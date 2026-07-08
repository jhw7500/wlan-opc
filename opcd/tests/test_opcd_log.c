/* opcd_log 순수 헬퍼 단위테스트 — ip4str / req·ind 이름표 / ack result peek /
 * init 전 no-op 계약. syslog 부작용 없는 경로만 검증한다. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../opcd_log.h"
#include "../../protocol/ids.h"

static void test_ip4str(void)
{
    char buf[16];
    assert(strcmp(opcd_ip4str(0xC0A8D605u, buf), "192.168.214.5") == 0);
    assert(strcmp(opcd_ip4str(0u, buf), "0.0.0.0") == 0);
    assert(strcmp(opcd_ip4str(0xFFFFFFFFu, buf), "255.255.255.255") == 0);
    assert(strcmp(opcd_ip4str(0u, NULL), "?") == 0);   /* NULL buf 방어 폴백 */
}

static void test_names(void)
{
    assert(strcmp(opcd_req_name(OPC_REQ_LOGIN), "Login") == 0);
    assert(strcmp(opcd_req_name(OPC_REQ_RESET), "Reset") == 0);
    assert(strcmp(opcd_req_name(OPC_REQ_SET_INDICATION_CONFIG), "SetIndicationConfig") == 0);
    assert(strcmp(opcd_req_name(0xBEEF), "Unknown") == 0);
    assert(strcmp(opcd_ind_name(OPC_IND_ROAMING), "Roaming") == 0);
    assert(strcmp(opcd_ind_name(OPC_IND_KEEP_ALIVE), "KeepAlive") == 0);
    assert(strcmp(opcd_ind_name(0x0040), "Unknown") == 0);
}

static void test_ack_result_peek(void)
{
    uint16_t res = 0xAAAA, cause = 0xBBBB;

    /* 정상 단순 ack: 64B 헤더(type=ACK) + Result NG(0x0001) + Cause 0x0010 */
    uint8_t ack[68] = {0};
    ack[0] = 0x01;   /* protocol_ver */
    ack[1] = 0x02;   /* command_type = ACK */
    ack[6] = 0x00; ack[7] = 0x3C;   /* Length = 60 */
    ack[64] = 0x00; ack[65] = 0x01; /* Result = NG */
    ack[66] = 0x00; ack[67] = 0x10; /* ErrorCause = 0x0010 */
    assert(opcd_ack_result_peek(ack, sizeof ack, &res, &cause) == 1);
    assert(res == 0x0001 && cause == 0x0010);

    /* OK ack */
    ack[65] = 0x00; ack[67] = 0x00;
    assert(opcd_ack_result_peek(ack, sizeof ack, &res, &cause) == 1);
    assert(res == 0x0000 && cause == 0x0000);

    /* GetBasicInfo ack(80B, id 0x0001) — result 필드 없음(vendor_code 선두) → 0 */
    uint8_t basic_ack[80] = {0};
    basic_ack[1] = 0x02;
    basic_ack[2] = 0x00; basic_ack[3] = 0x01;
    assert(opcd_ack_result_peek(basic_ack, sizeof basic_ack, &res, &cause) == 0);

    /* GetDeviceInfo ack(416B, id 0x0002) — result/error_cause 선두 → peek 성공.
     * (indication ON 중 NG 0x0010 응답도 감사에 남아야 한다 — PR #66 Codex P2) */
    uint8_t dev_ack[416] = {0};
    dev_ack[1] = 0x02;
    dev_ack[2] = 0x00; dev_ack[3] = 0x02;
    dev_ack[64] = 0x00; dev_ack[65] = 0x01;  /* NG */
    dev_ack[66] = 0x00; dev_ack[67] = 0x10;  /* indication-violation */
    assert(opcd_ack_result_peek(dev_ack, sizeof dev_ack, &res, &cause) == 1);
    assert(res == 0x0001 && cause == 0x0010);

    /* 타입 불일치(indication) → 0 */
    uint8_t ind[68] = {0};
    ind[1] = 0x03;
    assert(opcd_ack_result_peek(ind, sizeof ind, &res, &cause) == 0);

    /* NULL frame / 68B 미만 → 0 */
    assert(opcd_ack_result_peek(NULL, 68, &res, &cause) == 0);
    assert(opcd_ack_result_peek(ack, 67, &res, &cause) == 0);

    /* NULL 출력 인자 허용 */
    ack[1] = 0x02;
    assert(opcd_ack_result_peek(ack, sizeof ack, NULL, NULL) == 1);
}

static void test_noop_before_init(void)
{
    /* opcd_log_init() 미호출 상태 — 크래시/부작용 없이 조용히 무시돼야 한다.
     * (테스트 프로세스는 절대 init 하지 않는다 — 호스트 syslog 오염 방지 계약) */
    opcd_logf(LOG_INFO, "must be a no-op %d", 42);
    OLOG_INFO("macro path must be a no-op too (%s)", "x");
}

int main(void)
{
    test_ip4str();
    test_names();
    test_ack_result_peek();
    test_noop_before_init();
    printf("test_opcd_log: all tests passed\n");
    return 0;
}
