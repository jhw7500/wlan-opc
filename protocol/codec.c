#include <string.h>

#include "codec.h"

uint16_t opc_be16_read(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

uint32_t opc_be32_read(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24)
         | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8)
         | (uint32_t)p[3];
}

void opc_be16_write(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void opc_be32_write(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)v;
}

int opc_fixed_header_pack(uint8_t *buf, const opc_header_t *hdr)
{
    if (!buf || !hdr) {
        return -1;
    }
    buf[0] = hdr->protocol_version;
    buf[1] = hdr->command_type;
    opc_be16_write(&buf[2], hdr->req_indication_id);
    opc_be16_write(&buf[4], hdr->sequence_number);
    opc_be16_write(&buf[6], hdr->length);
    return 0;
}

int opc_header_pack(uint8_t *buf, const opc_header_t *hdr)
{
    if (!buf || !hdr) {
        return -1;
    }
    memset(buf, 0, OPC_HEADER_SIZE);   /* fixed 8 + reserve 56; bytes 8..63 stay zero */
    return opc_fixed_header_pack(buf, hdr);
}

int opc_fixed_header_unpack(const uint8_t *buf, size_t buf_len, opc_header_t *hdr)
{
    if (!buf || !hdr || buf_len < OPC_FIXED_HEADER_SIZE) {
        return -1;
    }
    hdr->protocol_version  = buf[0];
    hdr->command_type      = buf[1];
    hdr->req_indication_id = opc_be16_read(&buf[2]);
    hdr->sequence_number   = opc_be16_read(&buf[4]);
    hdr->length            = opc_be16_read(&buf[6]);
    return 0;
}

int opc_header_unpack(const uint8_t *buf, size_t buf_len, opc_header_t *hdr)
{
    if (!buf || !hdr || buf_len < OPC_HEADER_SIZE) {
        return -1;
    }
    return opc_fixed_header_unpack(buf, buf_len, hdr);
}
