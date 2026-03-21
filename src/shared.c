#include "shared.h"

void write_tlv(uint8_t **buf, tlv_tag_t tag, uint8_t len, const void *value, int convert_to_network) {
  
  // 1. Write tag and length (1 byte each, no flipping)
  *(*buf)++ = tag; // equivalent to **buf = tag, then (*buf)++
  *(*buf)++ = len;

  // 2. Write the convered (if necessary) byte seqeunce into the buffer
  if (convert_to_network) {
    // If we want to convert a multi-byte number into network-byte-order. Three cases:
    // a. Converting a 2-byte sequence
    // b. Converting a 4-byte sequence
    // c. Converting a 1-byte sequence (no conversion needed even if the caller wants it)
    if (len == 2) {
      uint16_t val;
      memcpy(&val, value, 2);
      val = htons(val);
      memcpy(*buf, &val, 2);
    } else if (len == 4) {
      uint32_t val;
      memcpy(&val, value, 4);
      val = htonl(val);
      memcpy(*buf, &val, 4);
    } else {
      memcpy(*buf, value, len);
    }
  } else {
    // Else, the caller intends to write a string into the buffer
    memcpy(*buf, value, len);
  }

  // 3. Advance buffer pointer by the byte size of the value.
  *buf += len;
}

int read_one_message(int connfd, message_t* msg) {
  
  // 1. Read message header
  uint8_t header[MSG_HEADER_SIZE];
  int bytes_to_read = MSG_HEADER_SIZE;
  uint8_t *buf_ptr = header;
  while (bytes_to_read > 0) {
    int bytes_read = read(connfd, buf_ptr, bytes_to_read);
    if (bytes_read == 0) {
      fprintf(stderr, "Unexpected EOF when reading msg header!\n");
      return -1;
    }
    if (bytes_read == -1) {
      fprintf(stderr, "read error when reading msg header: %s\n", strerror(errno));
      return -1;
    }
    buf_ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }

  // 2. Parse Header Fields into msg_t
  // a. bytes 0-2 are version, type, and flags
  // b. bytes 3-6 represent the payload length. A sequence of bytes like this
  // needs to be converted into host byte order.
  msg->version = msg->payload[0];
  msg->type = msg->payload[1];
  msg->flags = msg->payload[2];
  uint32_t payload_length;
  memcpy(&payload_length, &header[3], 4); 
  msg->payload_length = ntohl(payload_length);

  // 3. Validate payload size
  if (msg->payload_length > MSG_MAX_PAYLOAD_SIZE) {
    fprintf(stderr, "read_one_message: Payload of size '%d' is bigger than maximum!\n", msg->payload_length);
    return -1;
  }

  // 4. Read the message payload into msg->payload
  bytes_to_read = msg->payload_length;
  buf_ptr = (uint8_t *)msg->payload;
  while (bytes_to_read > 0) {
    int bytes_read = read(connfd, buf_ptr, bytes_to_read);
    if (bytes_read == 0) {
      fprintf(stderr, "Unexpected EOF when reading msg payload!\n");
      return -1;
    }
    if (bytes_read == -1) {
      fprintf(stderr, "read error when reading msg payload: %s\n", strerror(errno));
      return -1;
    }    
    buf_ptr += bytes_read;
    bytes_to_read -= bytes_read;
  }
  return 0;
}

int write_one_message(int connfd, message_t* response) { 
  uint8_t message_buffer[MSG_HEADER_SIZE+MSG_MAX_PAYLOAD_SIZE];

  // Copy fields; for multi-byte fields like payload_length ensure they're in big-endian
  uint8_t *buf_ptr = message_buffer;
  *buf_ptr++ = response->version;
  *buf_ptr++ = response->type;
  uint32_t net_len = htonl(response->payload_length);
  memcpy(buf_ptr, &net_len, sizeof(uint32_t));
  buf_ptr += sizeof(uint32_t);

  // Copy the main payload
  memcpy(buf_ptr, response->payload, response->payload_length);
  buf_ptr += response->payload_length;

  // Send it across socket

  // TODO: Wait this is wrong right? Or no because this is a blocking send
  size_t total_bytes = buf_ptr - message_buffer;
  int num_bytes_sent = send(connfd, message_buffer, total_bytes, 0);
  return num_bytes_sent;
}

int get_valid_input_range(char *prompt, int min, int max) {
  int value;
  int status;
  while (1) {
    printf(prompt);
    status = scanf("%d", &value);
    
    // 1. Handle non-numeric input
    // NOTE: Clears newline character from the buffer or until eof
    if (status != 1) {
      printf("Non-numeric input! Enter a NUMBER in range[%d, %d]: ", min, max);
      while (getchar() != '\n'); 
      continue;
    }

    // 2. Handle numeric range validation
    if (value < min || value > max) {
      printf("Out of range, enter a number in range[%d, %d]: ", min, max);
    } else {
      return value;
    }
  }
}