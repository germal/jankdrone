#include <SPI.h>
#include "logger.h"
#include "shm.h"
#include "config.h"
#include "remote.h"

Remote::Remote():
	m_firstRun{true},
	m_bluetooth{BLUEFRUIT_REQ_PIN, BLUEFRUIT_RDY_PIN, BLUEFRUIT_RST_PIN},
	m_lastState{ACI_EVT_DISCONNECTED} {}

void Remote::operator()() {
	// Must read bluetooth soon after begin()
	if (m_firstRun) {
		m_bluetooth.begin();
		m_firstRun = false;
	}

	readBluetooth();
	readStream(&Serial);
}

void Remote::readBluetooth() {
	m_bluetooth.pollACI();

	auto state = m_bluetooth.getState();
	if (state != m_lastState) {
		switch (state) {
			case ACI_EVT_DEVICE_STARTED:
				Logger::info("Bluetooth advertising started");
				break;
			case ACI_EVT_CONNECTED:
				Logger::info("Bluetooth connected!");
				break;
			case ACI_EVT_DISCONNECTED:
				Logger::info("Bluetooth disconnected or advertising timed out");
				break;
			default:
				// We don't care
				break;
		}
		m_lastState = state;
	}

	if (state == ACI_EVT_CONNECTED) {
		readStream(&m_bluetooth);
	}
}

void Remote::readStream(Stream* stream) {
	if (!stream->available()) return;

	int size = stream->read();
	int bytesRead = 0;
	while (bytesRead < size && stream->available()) {
		m_messageBuffer[bytesRead++] = stream->read();
	}
	if (bytesRead < size) {
		// Assume the whole message is available at once
		Logger::error("Remote message shorter than expected length, discarding");
		return;
	}

	ShmMsg msg = ShmMsg_init_zero;
	auto pbUpdateStream = pb_istream_from_buffer(m_messageBuffer, bytesRead);
	if (!pb_decode_noinit(&pbUpdateStream, ShmMsg_fields, &msg)) {
		Logger::error("Failed to decode remote message: {}",
				PB_GET_ERROR(&pbUpdateStream));
	}

	auto shmVar = Shm::varIfExists(msg.tag);
	if (!shmVar) {
		Logger::error("Remote var tag not found: {}", msg.tag);
		return;
	}

	Shm::Var::Type msgVarType;
	switch (msg.which_value) {
		case ShmMsg_intValue_tag:
			msgVarType = Shm::Var::Type::INT;
			break;
		case ShmMsg_floatValue_tag:
			msgVarType = Shm::Var::Type::FLOAT;
			break;
		case ShmMsg_boolValue_tag:
			msgVarType = Shm::Var::Type::BOOL;
			break;
		default:
			sendVar(stream, shmVar);
			return;
	}

	auto shmVarType = shmVar->type();
	if (msgVarType != shmVarType) {
		Logger::error("Remote var type mismatch: expected {}, got {}",
				Shm::typeString(shmVarType),
				Shm::typeString(msgVarType));
	}

	switch (shmVarType) {
		case Shm::Var::Type::INT:
			shmVar->set((int)msg.value.intValue);
			break;
		case Shm::Var::Type::FLOAT:
			shmVar->set(msg.value.floatValue);
			break;
		case Shm::Var::Type::BOOL:
			shmVar->set(msg.value.boolValue);
			break;
		default:
			Logger::error("Unsupported remote var type");
			break;
	}
}

void Remote::sendVar(Stream* stream, Shm::Var* var) {
	ShmMsg msg;
	msg.tag = var->tag();
	switch (var->type()) {
		case Shm::Var::Type::INT:
			msg.which_value = ShmMsg_intValue_tag;
			msg.value.intValue = var->getInt();
			break;
		case Shm::Var::Type::FLOAT:
			msg.which_value = ShmMsg_floatValue_tag;
			msg.value.floatValue = var->getFloat();
			break;
		case Shm::Var::Type::BOOL:
			msg.which_value = ShmMsg_boolValue_tag;
			msg.value.boolValue = var->getBool();
			break;
		default:
			Logger::error("Unsupported remote var type");
			return;
	}

	size_t encodedSize;
	pb_get_encoded_size(&encodedSize, ShmMsg_fields, &msg);
	constexpr int bufSize = sizeof(m_messageBuffer) / sizeof(m_messageBuffer[0]);
	if (encodedSize > (bufSize - 1)) {
		Logger::error("Encoded remote message too large to send");
		return;
	}

	m_messageBuffer[0] = (uint8_t)encodedSize;
	auto ostream = pb_ostream_from_buffer(&m_messageBuffer[1], bufSize - 1);
	pb_encode(&ostream, ShmMsg_fields, &msg);

	stream->write(m_messageBuffer, encodedSize+1);
}
