// Communication.cpp
#include "Communication.h"
#include <limits>  // For std::numeric_limits

#ifndef ARDUINO
#include <chrono>
#include <thread>
#endif

//#define VERBOSE
#define TIMEOUT_MS 1000  // Define timeout duration (1 second)

#ifndef ARDUINO
// Define Arduino constants for desktop builds
#define HEX 16
#endif

#define CRC32_POLYNOMIAL 0xEDB88320

// ----------------------------------------------------------------------------
// TX pacing (debug/compatibility)
//
// The known-good host upgrader (`upgrade_firmware.py`) transmits large packets
// in chunks with delays to avoid overflowing buffers on the target or the
// transport.
//
// This library historically wrote large payloads in a single Serial.write()
// call; if the link/target drops bytes, the device won't ACK.
//
// These knobs implement similar pacing for large writes.
// ----------------------------------------------------------------------------
#ifndef COMMUNICATION_PACE_THRESHOLD
#define COMMUNICATION_PACE_THRESHOLD 50
#endif

#ifndef COMMUNICATION_PACE_CHUNK_SIZE
// Split large writes into multiple Serial.write() calls, but do **not** add
// inter-chunk sleeps.
//
// Rationale: the target UART receive timeout is configured to ~0.1s in
// [`RS485.c`](../../Servomotor/common_source_files/RS485.c:159) via `USART1->RTOR`
// at [`RS485.c`](../../Servomotor/common_source_files/RS485.c:162). Any long gap
// between bytes can cause the target to reset its receive state and drop the
// packet (no ACK).
//
// Smaller chunks can still help with intermediate buffering (USB/UART driver)
// without introducing intentional gaps.
#define COMMUNICATION_PACE_CHUNK_SIZE 256
#endif

#ifndef COMMUNICATION_PACE_DELAY_MS
// Intentionally 0: no added delay between chunks.
#define COMMUNICATION_PACE_DELAY_MS 0
#endif

static inline void comm_delay_ms(uint32_t ms)
{
#ifdef ARDUINO
    delay(ms);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#endif
}

static uint32_t crc32_value;
static bool s_commSerialOpened = false;

// -----------------------------------------------------------------------------
// CRC32 implementation
//
// The original implementation used a bit-at-a-time CRC update (8 iterations per
// byte), which can be slow on microcontrollers and, critically, can create a
// long on-wire gap if CRC is computed *after* sending the payload and *before*
// sending the trailing CRC32.
//
// This table-driven implementation keeps the same polynomial/initialization and
// therefore remains wire-compatible, but is much faster (one table lookup per
// byte).
//
// Polynomial is the reflected CRC-32 (IEEE) polynomial used by the prior code:
// 0xEDB88320.
// -----------------------------------------------------------------------------
static uint32_t s_crc32_table[256];
static bool s_crc32_table_ready = false;

static void crc32_table_init()
{
    if (s_crc32_table_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (uint32_t j = 0; j < 8; j++) {
            if (c & 1u) {
                c = (c >> 1) ^ CRC32_POLYNOMIAL;
            } else {
                c = (c >> 1);
            }
        }
        s_crc32_table[i] = c;
    }
    s_crc32_table_ready = true;
}

// Debug helpers: hex-dump bytes to Serial.
// These are used by the optional TX/RX dumps.
static void print_hex_byte(uint8_t b)
{
    Serial.print("0x");
    if (b < 0x10) {
        Serial.print("0");
    }
    Serial.print(b, HEX);
}

static void dump_hex_bytes_with_wrap(uint32_t &pos, const uint8_t *data, size_t len, uint8_t wrap = 16)
{
    for (size_t i = 0; i < len; i++) {
        print_hex_byte(data[i]);
        Serial.print(" ");
        pos++;
        if (wrap != 0 && (pos % wrap) == 0) {
            Serial.println();
        }
    }
}

static void dump_hex_byte_with_wrap(uint32_t &pos, uint8_t b, uint8_t wrap = 16)
{
    print_hex_byte(b);
    Serial.print(" ");
    pos++;
    if (wrap != 0 && (pos % wrap) == 0) {
        Serial.println();
    }
}

// Enable/disable RX dumping here.
// If your build already defines VERBOSE, you can keep this on; it is intentionally very noisy.
#ifndef COMMUNICATION_DUMP_RX
#define COMMUNICATION_DUMP_RX 1
#endif

// Enable/disable TX dumping here.
// IMPORTANT: dumping TX bytes during a large packet can *block* on USB Serial
// and introduce large on-wire gaps (e.g. right before the CRC32 trailer),
// which can trip the target's UART RX timeout and make it drop the packet.
// Keep this OFF during real firmware upgrades.
#ifndef COMMUNICATION_DUMP_TX
#define COMMUNICATION_DUMP_TX 0
#endif

void crc32_init(void)
{
    crc32_table_init();
    crc32_value = 0xFFFFFFFF;
}

uint32_t calculate_crc32_buffer_without_reinit(const void* data, size_t length)
{
    const uint8_t* d = (const uint8_t*)data;
    for (size_t i = 0; i < length; i++) {
        const uint8_t idx = (uint8_t)((crc32_value ^ d[i]) & 0xFFu);
        crc32_value = (crc32_value >> 8) ^ s_crc32_table[idx];
    }
    return ~crc32_value;
}

uint32_t calculate_crc32(const uint8_t* data, size_t length)
{
    crc32_init();
    return calculate_crc32_buffer_without_reinit(data, length);
}

uint32_t get_crc32(void)
{
    return ~crc32_value;
}

Communication::Communication(HardwareSerial& serialPort, uint32_t baud, int8_t rxPin, int8_t txPin)
    : _serial(serialPort),
      _crc32Enabled(true),
      _baud(baud),
      _rxPin(rxPin),
      _txPin(txPin) {
    // No initialization needed for direct CRC32 calculation
}

void Communication::openSerialPort() {
#ifdef ARDUINO
    // Initialize the RS485 hardware serial port once (even if multiple Servomotor instances exist)
    if (!s_commSerialOpened) {
        #if defined(ESP32)
        if (_rxPin >= 0 && _txPin >= 0) {
            _serial.begin(_baud, SERIAL_8N1, _rxPin, _txPin);
        } else {
            _serial.begin(_baud);
        }
        #else
        _serial.begin(_baud);
        #endif
        s_commSerialOpened = true;
    }
#else
    // On desktop (ArduinoEmulator), Serial1 is initialized in ArduinoEmulator.cpp main()
#endif
}

void Communication::sendCommand(uint8_t alias, uint8_t commandID, const uint8_t* payload, uint16_t payloadSize) {
    sendCommandCore(false, alias, commandID, payload, payloadSize);
}

void Communication::sendCommandByUniqueId(uint64_t uniqueId, uint8_t commandID, const uint8_t* payload, uint16_t payloadSize) {
    sendCommandCore(true, uniqueId, commandID, payload, payloadSize);
}

void Communication::sendCommandCore(bool isExtendedAddress, uint64_t addressValue, uint8_t commandID,
                                   const uint8_t* payload, uint16_t payloadSize) {
    
    uint8_t sizeByte;
    bool isExtendedSize = false;
    uint16_t size16Bit;

    // TX dumping is optional and OFF by default to avoid changing on-wire timing.
    #if COMMUNICATION_DUMP_TX
    uint32_t tx_pos = 0;
    Serial.println("TX bytes (each _serial.write in order):");
    #define TX_DUMP_BYTE(b) do { dump_hex_byte_with_wrap(tx_pos, (uint8_t)(b)); } while (0)
    #define TX_DUMP_BUF(ptr, len) do { dump_hex_bytes_with_wrap(tx_pos, (const uint8_t *)(ptr), (len)); } while (0)
    #else
    #define TX_DUMP_BYTE(b) do { } while (0)
    #define TX_DUMP_BUF(ptr, len) do { } while (0)
    #endif

    #define TX_WRITE_BYTE(b) do { _serial.write((uint8_t)(b)); TX_DUMP_BYTE((uint8_t)(b)); } while (0)

    // Pacing for larger buffers: write in chunks with delays.
    const auto write_buf_paced = [&](const uint8_t *ptr, size_t len) {
        if (!ptr || len == 0) return;
        if (len <= (size_t)COMMUNICATION_PACE_THRESHOLD) {
            _serial.write(ptr, len);
            return;
        }

        for (size_t off = 0; off < len; off += (size_t)COMMUNICATION_PACE_CHUNK_SIZE) {
            const size_t take = ((len - off) > (size_t)COMMUNICATION_PACE_CHUNK_SIZE) ? (size_t)COMMUNICATION_PACE_CHUNK_SIZE : (len - off);
            _serial.write(ptr + off, take);
            // No intentional delay here (see COMMUNICATION_PACE_DELAY_MS comment).
            if (COMMUNICATION_PACE_DELAY_MS > 0 && (off + take) < len) {
                comm_delay_ms((uint32_t)COMMUNICATION_PACE_DELAY_MS);
            }
        }
    };

    #define TX_WRITE_BUF(ptr, len) do { write_buf_paced((const uint8_t *)(ptr), (len)); TX_DUMP_BUF((const uint8_t *)(ptr), (len)); } while (0)
    // Calculate address size
    const uint16_t addressSize = isExtendedAddress ? (sizeof(uint8_t) + sizeof(uint64_t)) : sizeof(uint8_t); // Extended: 1 byte flag + 8 bytes ID, Standard: 1 byte alias
    
    // Calculate total packet size (initial calculation without extended size bytes)
    uint32_t totalPacketSize = sizeof(sizeByte) + addressSize + sizeof(commandID) + payloadSize;
    if (_crc32Enabled) {
        totalPacketSize += sizeof(uint32_t); // Add 4 bytes for CRC32
    }
    
    // Determine size bytes WITHOUT transmitting yet.
    if (totalPacketSize <= DECODED_FIRST_BYTE_EXTENDED_SIZE) {
        sizeByte = encodeFirstByte(totalPacketSize);
    } else {
        isExtendedSize = true;
        sizeByte = encodeFirstByte(DECODED_FIRST_BYTE_EXTENDED_SIZE);
        totalPacketSize += sizeof(uint16_t); // Add 2 bytes for the extended size
        if (totalPacketSize > std::numeric_limits<uint16_t>::max()) {
            #ifdef VERBOSE
            Serial.println("Packet larger than protocol supports. Nothing being transmitted.");
            #endif
            return;
        }
        size16Bit = (uint16_t)totalPacketSize;
    }

    // Precompute CRC BEFORE any TX so there is no gap before the CRC trailer.
    uint32_t crc = 0;
    if (_crc32Enabled) {
        crc32_init();
        calculate_crc32_buffer_without_reinit(&sizeByte, sizeof(sizeByte));
        if (isExtendedSize) {
            calculate_crc32_buffer_without_reinit(&size16Bit, sizeof(size16Bit));
        }
        if (isExtendedAddress) {
            const uint8_t extendedAddrByte = EXTENDED_ADDRESSING;
            calculate_crc32_buffer_without_reinit(&extendedAddrByte, sizeof(extendedAddrByte));
            calculate_crc32_buffer_without_reinit(&addressValue, sizeof(addressValue));
        } else {
            const uint8_t alias = (uint8_t)addressValue;
            calculate_crc32_buffer_without_reinit(&alias, sizeof(alias));
        }
        calculate_crc32_buffer_without_reinit(&commandID, sizeof(commandID));
        if (payload != nullptr && payloadSize > 0) {
            calculate_crc32_buffer_without_reinit(payload, payloadSize);
        }
        crc = get_crc32();
    }

    // Now transmit bytes back-to-back.
    TX_WRITE_BYTE(sizeByte);
    if (isExtendedSize) {
        TX_WRITE_BUF((uint8_t *)&size16Bit, sizeof(size16Bit));
    }
    
    // Write address bytes
    if (isExtendedAddress) {
        const uint8_t extendedAddrByte = EXTENDED_ADDRESSING;
        TX_WRITE_BYTE(extendedAddrByte);
        TX_WRITE_BUF((uint8_t *)&addressValue, sizeof(addressValue));
    }
    else {
        const uint8_t alias = (uint8_t)addressValue;
        TX_WRITE_BYTE(alias);
    }
    
    // Write command byte
    TX_WRITE_BYTE(commandID);
    
    #ifdef VERBOSE
    if (isExtendedAddress) {
        Serial.print("Sent extended command with ");
        Serial.print(_crc32Enabled ? "CRC32 enabled" : "CRC32 disabled");
        Serial.print(", uniqueId: 0x");
        // Print Unique ID in big-endian format for readability
        for (int i = 7; i >= 0; i--) {
            uint8_t byte = (addressValue >> (i * 8)) & 0xFF;
            if (byte < 0x10) Serial.print("0");
            Serial.print(static_cast<int>(byte), HEX);
        }
        Serial.print(", command: 0x");
        if (commandID < 0x10) Serial.print("0");
        Serial.println(commandID, HEX);
        Serial.println();
    }
    #endif

    // CRC already computed above.
    
    // Write payload
    if (payload != nullptr && payloadSize > 0) {
        TX_WRITE_BUF(payload, payloadSize);
    }
    else {
        #ifdef VERBOSE
        Serial.print("No payload");
        if (isExtendedAddress) Serial.print(" (extended address)");
        Serial.println();
        #endif
    }
    
    // Calculate and write CRC32 if enabled
    if (_crc32Enabled) {
        TX_WRITE_BUF((uint8_t*)(&crc), sizeof(crc));
    }

    #if COMMUNICATION_DUMP_TX
    if ((tx_pos % 16) != 0) {
        Serial.println();
    }
    Serial.println("TX end");
    #endif

    #undef TX_DUMP_BYTE
    #undef TX_DUMP_BUF
    #undef TX_WRITE_BYTE
    #undef TX_WRITE_BUF
}

int16_t Communication::getResponse(uint8_t* buffer, uint16_t bufferSize, uint16_t& receivedSize) {
    uint32_t startTime = millis();
    bool isExtendedSize = false;
    uint8_t sizeBytes[3];
    uint8_t sizeByteCount = 1;
    int32_t packetSize = 0;
    int16_t error_code = 0;
    int32_t bytesLeftToRead = 0;
    uint8_t remoteErrorCode = 0;
    uint8_t remoteErrorCodePresent = 0;
    int32_t bytesLeftToReadWithoutTheCRC32;

    // Read first (encoded) size byte. From it we will determine the packet size or determine it to indicate extended size
    if((error_code = receiveBytes(&(sizeBytes[0]), 1, 1, TIMEOUT_MS - (millis() - startTime))) != 0) {
        return error_code;
    }

    #ifdef VERBOSE
    Serial.print("Received first byte: 0x");
    if (sizeBytes[0] < 0x10) Serial.print("0");
    Serial.println(sizeBytes[0], HEX);
    Serial.print("Binary: ");
    for (int i = 7; i >= 0; i--) {
        Serial.print((sizeBytes[0] >> i) & 1);
    }
    Serial.println();
    #endif

    // Validate first byte format (LSB must be 1)
    if (!isValidFirstByteFormat(sizeBytes[0])) {
        #ifdef VERBOSE
        Serial.println("Invalid first byte format (LSB not 1)");
        #endif
        return COMMUNICATION_ERROR_BAD_FIRST_BYTE;
    }

    // Decode size from first byte
    uint8_t decodedSize = decodeFirstByte(sizeBytes[0]);

    #ifdef VERBOSE
    Serial.print("Decoded size: ");
    Serial.println(decodedSize);
    #endif

    // Check if we have extended size format
    if (decodedSize == DECODED_FIRST_BYTE_EXTENDED_SIZE) {
        isExtendedSize = true;

        // We determined that the size byte indicates an extended size where the size is incoded in 16-bite. Read them.
        uint16_t extendedSize;
        if((error_code = receiveBytes(&extendedSize, sizeof(extendedSize), sizeof(extendedSize), TIMEOUT_MS - (millis() - startTime))) != 0) {
            return error_code;
        }
        sizeBytes[1] = ((uint8_t*)&extendedSize)[0];
        sizeBytes[2] = ((uint8_t*)&extendedSize)[1];
        sizeByteCount = 3;

        #ifdef VERBOSE
        Serial.print("Extended size bytes: 0x");
        if (sizeBytes[1] < 0x10) Serial.print("0");
        Serial.print(sizeBytes[1], HEX);
        Serial.print(" 0x");
        if (sizeBytes[2] < 0x10) Serial.print("0");
        Serial.println(sizeBytes[2], HEX);
        #endif

        bytesLeftToRead = extendedSize - sizeByteCount;
    } else {
        bytesLeftToRead = decodedSize - sizeByteCount;
    }

    #ifdef VERBOSE
    Serial.print("The number of bytes left to read are: ");
    Serial.println(bytesLeftToRead);
    #endif

    if(bytesLeftToRead < 1) { // we need at least one extra byte beyond the size byte(s)
        #ifdef VERBOSE
        Serial.println("The packet size is too small to be valid");
        #endif
        error_code = COMMUNICATION_ERROR_PACKET_TOO_SMALL;
        goto flush_read_remaining_bytes_and_return_error;
    }
        
    // Read response character
    uint8_t responseChar;
    if((error_code = receiveBytes(&responseChar, 1, 1, TIMEOUT_MS - (millis() - startTime))) != 0) {
        goto flush_read_remaining_bytes_and_return_error;
    }
    #ifdef VERBOSE
    Serial.print("Response character: 0x");
    if (responseChar < 0x10) Serial.print("0");
    Serial.println(responseChar, HEX);
    #endif
    // Calculate the payload size (everything after response character)
    // This includes the error code byte plus actual data
    bytesLeftToRead--; // Subtract 1 for the response character
    
    if ((responseChar != RESPONSE_CHARACTER_CRC32_ENABLED) && (responseChar != RESPONSE_CHARACTER_CRC32_DISABLED)) {
        #ifdef VERBOSE
        Serial.print("Invalid response character. Expected 0x"); // DEBUG
        Serial.print(RESPONSE_CHARACTER_CRC32_ENABLED, HEX);
        Serial.print(" or 0x");
        Serial.print(RESPONSE_CHARACTER_CRC32_DISABLED, HEX);
        Serial.print(", but got 0x");
        Serial.println(responseChar, HEX); // DEBUG
        #endif
        error_code = COMMUNICATION_ERROR_BAD_RESPONSE_CHAR;
        goto flush_read_remaining_bytes_and_return_error;
    }

    bytesLeftToReadWithoutTheCRC32 = bytesLeftToRead;
    if (responseChar == RESPONSE_CHARACTER_CRC32_ENABLED) {
        if(bytesLeftToRead < 4) { // we need at least 4 bytes for the CRC32
            #ifdef VERBOSE
            Serial.println("The packet size is too small to hold the expected 4-byte CRC32 value");
            #endif
            error_code = COMMUNICATION_ERROR_PACKET_TOO_SMALL;
            goto flush_read_remaining_bytes_and_return_error;
        }
        bytesLeftToReadWithoutTheCRC32 -= 4; // Subtract 4 for the CRC32 size if it's enabled
    }
    if ((bytesLeftToReadWithoutTheCRC32 == 0) && (bufferSize != 0)) {
        error_code = COMMUNICATION_ERROR_DATA_WRONG_SIZE;
        goto flush_read_remaining_bytes_and_return_error;
    }

    // Now receive the error code byte from the remote device if there are enough bytes left
    if (bytesLeftToReadWithoutTheCRC32 >= 1) {
        if((error_code = receiveBytes(&remoteErrorCode, sizeof(remoteErrorCode), sizeof(remoteErrorCode), TIMEOUT_MS - (millis() - startTime))) != 0) {
            goto flush_read_remaining_bytes_and_return_error;
        }
        remoteErrorCodePresent = 1;
        if (remoteErrorCode != 0) {
            error_code = remoteErrorCode;
            goto flush_read_remaining_bytes_and_return_error;
        }
        bytesLeftToReadWithoutTheCRC32--; // Subtract 1 for the remote error code that we just read
    }

    // Read the payload
    if((error_code = receiveBytes(buffer, bufferSize, bytesLeftToReadWithoutTheCRC32, TIMEOUT_MS - (millis() - startTime))) != 0) {
        goto flush_read_remaining_bytes_and_return_error;
    }
    receivedSize = bytesLeftToReadWithoutTheCRC32;

    // Receive the CRC32 if there is one
    if (responseChar == RESPONSE_CHARACTER_CRC32_ENABLED) {
        uint32_t crc32 = 0;
        if((error_code = receiveBytes(&crc32, sizeof(crc32), sizeof(crc32), TIMEOUT_MS - (millis() - startTime))) != 0) {
            goto flush_read_remaining_bytes_and_return_error;
        }
        // And also calculate the CRC32 and compare to make sure that the received CRC32 is correct. Include all bytes, including the size bytes, in the calculation except don't include the CRC32 bytes themselves.
        calculate_crc32(sizeBytes, sizeByteCount);
        calculate_crc32_buffer_without_reinit(&responseChar, sizeof(responseChar));
        if (remoteErrorCodePresent) {
            calculate_crc32_buffer_without_reinit(&remoteErrorCode, sizeof(remoteErrorCode));
        }
        uint32_t calculated_crc32 = calculate_crc32_buffer_without_reinit(buffer, bytesLeftToReadWithoutTheCRC32);
        if (calculated_crc32 != crc32) {
            #ifdef VERBOSE
            Serial.print("CRC32 mismatch! Calculated: 0x");
            Serial.print(calculated_crc32, HEX);
            Serial.print(", Received: 0x");
            Serial.println(crc32, HEX);
            #endif
            error_code = COMMUNICATION_ERROR_CRC32_MISMATCH;
            goto flush_read_remaining_bytes_and_return_error;
        }
    }

    return COMMUNICATION_SUCCESS;

/*
    // Always read full payload (needed for CRC32 calculation)
    // receiveBytes will handle buffer overflow gracefully
    if (payloadSize > 0) {
        // We are expecting more bytes. Two cases:
        // (1) there is just one more byte, and that should be an error code or 0 if no error
        // (2) there is more than one byte, and we need to read the error code first (one byte) and then we need to read the entire payload into the given receive buffer
        #ifdef VERBOSE
        Serial.print("There is an error status byte present and we need to read it now. After that error status byte, there are ");
        Serial.print(payloadSize - 1);
        Serial.println(" more bytes (which is the payload)");
        #endif
        if((result = receiveBytes(buffer, bufferSize, payloadSize)) != 0) {
            #ifdef VERBOSE
            Serial.println("An error occured while receiving");
            Serial.print("Error code: ");
            Serial.println(result);
            #endif
            return result;
        }
        
        #ifdef VERBOSE
        Serial.print("Waiting for ");
        Serial.print(payloadSize);
        Serial.println(" more bytes (which is the size of the payload)");
        Serial.print("Received full payload of ");
        Serial.print(payloadSize);
        Serial.println(" bytes");
        Serial.println("Raw packet data (hex):");
        uint16_t printSize = (buffer != nullptr) ? payloadSize : 0;
        for (uint16_t i = 0; i < printSize; i++) {
            Serial.print("0x");
            if (buffer[i] < 0x10) Serial.print("0");
            Serial.print(buffer[i], HEX);
            Serial.print(" ");
            if ((i + 1) % 8 == 0) Serial.println();
        }
        if (printSize % 8 != 0) Serial.println();
        #endif
    }
    
    #ifdef VERBOSE
    Serial.print("Received full payload of ");
    Serial.print(payloadSize);
    Serial.println(" bytes");
    Serial.println("Raw packet data (hex):");
    for (uint16_t i = 0; i < payloadSize; i++) {
        Serial.print("0x");
        if (buffer[i] < 0x10) Serial.print("0");
        Serial.print(buffer[i], HEX);
        Serial.print(" ");
        if ((i + 1) % 8 == 0) Serial.println();
    }
    Serial.println();
    #endif

    // Validate CRC32 if the response indicates it's enabled
    if (responseChar == RESPONSE_CHARACTER_CRC32_ENABLED) {
        // Receive the four bytes for the CRC32
        uint32_t waitStartTime = millis();
        while (_serial.available() < 4) {
            if (millis() - waitStartTime > TIMEOUT_MS) {
                #ifdef VERBOSE
                Serial.print("Timed out waiting for CRC32. Expected "); // DEBUG
                Serial.print(4); // DEBUG
                Serial.print(" more bytes, but only "); // DEBUG
                Serial.print(_serial.available()); // DEBUG
                Serial.println(" available."); // DEBUG
                #endif
                return COMMUNICATION_ERROR_TIMEOUT;
            }
        }

        // Extract CRC32 from the received bytes
        uint32_t receivedCRC = 0;
        receivedCRC |= _serial.read();
        receivedCRC |= _serial.read() << 8;
        receivedCRC |= _serial.read() << 16;
        receivedCRC |= _serial.read() << 24;

        #ifdef VERBOSE
        Serial.print("Received CRC32: 0x");
        Serial.println(receivedCRC, HEX);
        #endif

        // Calculate CRC32 of the packet data (excluding the CRC32 itself)
        // This matches the Python implementation: size_bytes + packet_data[:-4]
        uint8_t crcData[payloadSize + 4]; // max: 3 size + 1 response
        memcpy(crcData, sizeBytes, sizeByteCount);
        crcData[sizeByteCount] = responseChar;
        if (payloadSize > 0) {
            memcpy(crcData + sizeByteCount + 1, buffer, payloadSize);
        }
        uint32_t calculatedCRC = calculate_crc32(crcData, sizeByteCount + 1 + payloadSize);

        #ifdef VERBOSE
        Serial.print("Calculated CRC32: 0x");
        Serial.println(calculatedCRC, HEX);
        #endif

        // Verify CRC32
        if (receivedCRC != calculatedCRC) {
            #ifdef VERBOSE
            Serial.print("CRC32 mismatch. Received: "); // DEBUG
            Serial.print(static_cast<int>(receivedCRC), HEX); // DEBUG
            Serial.print(", Calculated: "); // DEBUG
            Serial.println(static_cast<int>(calculatedCRC), HEX); // DEBUG
            #endif
            return COMMUNICATION_ERROR_CRC32_MISMATCH;
        }
    }
    
    // Process error code and adjust payload
    if (payloadSize == 0) {
        // Case 1: Empty payload - success response
        #ifdef VERBOSE
        Serial.println("Empty payload - success response");
        #endif
        receivedSize = 0;
        return COMMUNICATION_SUCCESS;
    }
    
    // Extract error code from first byte of payload
    // Note: we might not have the data if buffer was too small, but we have error code
    uint8_t errorCode = (buffer != nullptr) ? buffer[0] : 0; // Default to success if no buffer
    #ifdef VERBOSE
    Serial.print("Error code in payload: ");
    Serial.println(errorCode);
    #endif
    
    if (payloadSize == 1) {
        // Case 2: Only error code byte
        receivedSize = 0;
        return (errorCode == 0) ? COMMUNICATION_SUCCESS : -errorCode;
    }
    
    // Case 3: Error code + data bytes
    uint16_t dataSize = payloadSize - 1;
    
    // Check if buffer can hold final data size
    if (buffer != nullptr && bufferSize < dataSize) {
        #ifdef VERBOSE
        Serial.print("Buffer too small for final data. Need ");
        Serial.print(dataSize);
        Serial.print(" bytes, buffer is ");
        Serial.println(bufferSize);
        #endif
        receivedSize = 0;
        return COMMUNICATION_ERROR_BUFFER_TOO_SMALL;
    }
    
    // If we have a buffer and it was big enough for data, move data
    if (buffer != nullptr && bufferSize >= dataSize) {
        memmove(buffer, buffer + 1, dataSize);
        receivedSize = dataSize;
    } else {
        receivedSize = 0;
    }
    
    #ifdef VERBOSE
    Serial.print("Actual payload size after removing error code: ");
    Serial.println(receivedSize);
    #endif
    
    return (errorCode == 0) ? COMMUNICATION_SUCCESS : -errorCode;
*/
flush_read_remaining_bytes_and_return_error:
    if (bytesLeftToRead > 0) {
        receiveBytes(nullptr, 0, bytesLeftToRead, TIMEOUT_MS - (millis() - startTime));
    }
    return error_code;
}

void Communication::flush() {
    _serial.flush(); // Flush any outgoing data
    while (_serial.available()) {
        _serial.read(); // Clear any incoming data
    }
}

int8_t Communication::receiveBytes(void* buffer, uint16_t bufferSize, int32_t numBytes, int32_t timeout_ms) {
    if (numBytes == 0) {
        return COMMUNICATION_SUCCESS;
    }

    if (numBytes < 0) {
        return COMMUNICATION_ERROR_PACKET_TOO_SMALL;
    }


    bool bufferTooSmall = (buffer != nullptr && bufferSize < numBytes);

    #if COMMUNICATION_DUMP_RX
    static uint32_t rx_pos = 0;
    Serial.print("RX receiveBytes(): want=");
    Serial.print(numBytes);
    Serial.print(" timeout_ms=");
    Serial.print(timeout_ms);
    Serial.print(" bufferSize=");
    Serial.print(bufferSize);
    Serial.print(" store=");
    Serial.println((buffer != nullptr && !bufferTooSmall) ? "yes" : "no");
    #endif

    // Wait for all bytes to arrive
    uint32_t startTime = millis();
    while (_serial.available() < numBytes) {
        if (millis() - startTime > timeout_ms) {
            #if COMMUNICATION_DUMP_RX
            Serial.print("RX timeout waiting for ");
            Serial.print(numBytes);
            Serial.print(" bytes. available=");
            Serial.print(_serial.available());
            Serial.print(" elapsed_ms=");
            Serial.print(millis() - startTime);
            Serial.print(" timeout_ms=");
            Serial.println(timeout_ms);
            #endif
            return COMMUNICATION_ERROR_TIMEOUT;
        }
    }

    // Read all bytes (store in buffer if adequate, otherwise discard)
    for (uint16_t i = 0; i < numBytes; i++) {
        uint8_t byte = _serial.read();

        #if COMMUNICATION_DUMP_RX
        dump_hex_byte_with_wrap(rx_pos, byte);
        #endif

        if (buffer != nullptr && !bufferTooSmall) {
            ((char*)buffer)[i] = byte;
        }
    }

    #if COMMUNICATION_DUMP_RX
    if ((rx_pos % 16) != 0) {
        Serial.println();
    }
    Serial.println("RX end");
    #endif

    if(bufferTooSmall) {
        #ifdef VERBOSE
        Serial.print("Buffer too small for final data. Need ");
        Serial.print(numBytes);
        Serial.print(" bytes, buffer is ");
        Serial.println(bufferSize);
        #endif
        return COMMUNICATION_ERROR_BUFFER_TOO_SMALL;
    }
    else {
        return COMMUNICATION_SUCCESS;
    }
}

void Communication::enableCRC32() {
    _crc32Enabled = true;
}

void Communication::disableCRC32() {
    _crc32Enabled = false;
}

bool Communication::isCRC32Enabled() const {
    return _crc32Enabled;
}
