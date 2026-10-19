# wlan-opc 미구현 항목 현황 (Implementation Status)

**조사일**: 2026-06-16 · **방법**: 코드 마커 grep(no-op/V3/pending/canned/TODO/deferred) + `spec-conformance.md` + proto-todo 전수 교차 (16건 발견 → 묶음 정리)
**분류**: ① 자율 구현 가능(외부 대기 없음) · ② 발주처/벤더/spec 대기 · ③ 구현됨·실측만(미구현 아님)
**항목번호**: `spec-conformance.md`의 V#/D#/③-B, proto-todo의 T# 참조

---

## 0.7.0 배포 방침 (2026-10-13 사용자 결정)

- **출하 IP**: 양산 프로그램 또는 전용 이미지에서 현장별로 설정한다. 이번 공통 패키지에서 출하 IP나 동일-IP/분리-IP 프로파일을 일괄 고정하지 않는다. 양산에서 선택한 프로파일의 설치·부팅·ChangeIp 동작 검증은 별도 범위다.
- **사양에 없는 Error Cause**: 당사 정의값으로 사용하고, 의미와 적용 명령을 정리한 스펙 문서를 후속으로 전달한다. 아래 `0x0050`은 DFK 정식 사양값으로 승인됐다는 뜻이 아니며, 해당 명령 안에서 해석한다. 발주처의 값 확정 회신을 이번 구현의 선행 조건으로 두지 않는다.

| Command | Error Cause | Meaning |
|---|---|---|
| SetRadioConfig | 0x0050 | Runtime apply failure after input validation |
| ChangeIpAddress | 0x0050 | Subnet clash with the other management interface |

- **장애 통지와 리셋**: CPU·Disk·Network 사용률 폭주의 FaultDetect(0x0010)만으로 자동 리셋하지 않는다. 커널·펌웨어·드라이버 등으로 실제 운용이 불가능해진 경우에는 기존 복구·재부팅 정책을 따르며 Reset Cause 기록 → ResetNotice(0x0020) 송신 시도 → 리셋 경로를 사용한다. AP 미접속이나 일시적인 사용률 상승 자체를 새 즉시 리셋 조건으로 추가하지 않는다.
- **통지의 실행 범위**: ResetNotice는 OPCd·커널·통신 경로가 동작하고 해당 통지가 설정된 경우에 송신을 시도한다. 커널 완전 정지·송신 경로 소실·HW watchdog/강제 리셋에서는 사전 통지나 상대 수신을 보장할 수 없으므로 복구를 우선한다.
- **고장 부위별 경로 (사용자 후속 지침)**: 무선·펌웨어·드라이버 장애와 이더넷/커널 장애를 구분하여 검증한다. 무선이 고장 나도 수신기까지의 유선 경로와 OPCd가 살아 있으면 ResetNotice 송신을 시도할 수 있다. 현재 UDP는 wildcard bind와 일반 `sendto`를 사용하며 OS 라우팅을 따른다. 이더넷이 존재한다는 사실만으로 도달 가능하거나 자동으로 경로를 전환한다고 보지 않는다. OPCd 종료 시에는 `Restart=always`로 서비스가 재기동하며, 메인 루프의 1초 heartbeat와 30초 systemd watchdog으로 살아 있는 채 멈춘 OPCd도 서비스만 재시작한다. 저장 worker가 heartbeat를 대신 보내지 않으며 자동 보드 리셋 조건을 연결하지 않았다. 검증 시나리오는 `../testing/testing-guide.md`의 고장 부위별 항목을 참조한다.
- **재부팅 fallback 보완**: 타겟의 독립 시험 서비스를 정지시키자 기존 reboot policy 자식이 함께 종료되어 다음 단계가 실행되지 않는 결함을 재현했다. 전체 정책을 `DefaultDependencies=no`인 별도 transient service에서 한 번만 실행하도록 변경했고, 호출 서비스 정지 후에도 강제 재부팅/sysrq 대체 표식이 순서대로 실행됨을 확인했다. 실제 강제 리셋이나 최종 shutdown의 전체 프로세스 종료·커널 완전 정지를 시험한 것은 아니다. ResetNotice보다 supplicant/무선 경로가 먼저 종료될 가능성도 별도 제약이다.
- **R1/R2 실기 결과 (2026-10-13)**: 현재 `eth0_ip` 구성에서 AP 연결을 끊어도 유선 WlanStatusChange/KeepAlive가 계속 수신됐고 boot ID가 유지됐다. 원래 AP로 재연결한 뒤 mlan0를 DOWN/INTERFACE_DISABLED 상태로 만들고, 기존 정책을 `wlan_fw_watch` source로 수동 호출하여 cause 0x12 ResetNotice 1회가 유선 수신기에 전달된 뒤 정상 재부팅 1회가 완료됨을 확인했다. 실제 FW/driver 크래시나 자동 검출을 주입한 시험은 아니다. 원본 파일·드라이버·MAC·IP·경로·전체 기록 서비스의 active/enable 상태와 무선 ping을 fresh audit으로 확인했다. 해당 R1/R2 시험에서는 강제 리셋 fallback·OPCd hang·다른 양산 프로파일을 검증하지 않았으며, 서비스 생존성과 OPCd 재시작의 후속 결과는 별도로 기록한다. 상세는 testing-guide의 R1/R2 실기 결과와 부모 저장소 `.omx/plans/opcd-reset-notice-20261013-v6jp9i/qa-summary.json`에 있다.
- **R4/R5 후속 결과 (2026-10-13)**: 실제 타겟의 격리된 ARM64 stub OPCd에서 정상 32초 heartbeat(재시작 0회), SIGKILL 후 2.582초 내 응답 복구, SIGSTOP 후 PID 1의 watchdog timeout과 31.233초 내 응답 복구를 확인했다. 두 재시작 후 로그인과 통지 수신기 설정이 초기화되어 재로그인·재설정하면 KeepAlive가 다시 전달됐다. 최초 시험의 임시 영역 격리 오류는 원인 확인·정리·수정 후 재시험했고, 원본 파일·서비스·MAC/IP/경로/AP/boot ID 및 양 경로 ping을 최종 대조했다. 일시적인 SSH 연결 중단의 원인은 미확정이다. 정책 관련 pytest 42건·manifest 2건·단독 전체 native check·ARM64 NXP 빌드가 통과했다. 타겟 원본은 유지하고 새 NXP 바이너리/unit은 로컬 staging에만 반영했다. 증거는 부모 저장소 `.omx/plans/opcd-recovery-watchdog-20261013-0s04gpcz/`와 testing-guide에 있다.
- **R6/R7 후속 결과 (2026-10-13)**: 격리된 ARM64 stub에서 CPU/Disk/Network 카운터를 주입하여 진입·재진입 FaultDetect 6건, 지속·해제 중 추가 통지 0건, ResetNotice·서비스 재시작·보드 재부팅 0회를 확인했다. 실제 자원 포화 시험은 아니다. 저장·로그 실패는 타겟의 시험용 정책 사본으로 18건을 검증했다. 로그·stderr·sync·상태 읽기/쓰기·원인 저장/삭제에 제한 시간을 적용하고, TERM으로 wrapper만 먼저 끝나 손자가 남는 결함을 I/O 그룹의 초기 SIGKILL로 수정했다. 정상 제조 모드 거부 12와 기존 예외는 유지하며 판정 불가/잘못된 값은 안전 거부 13으로 보고한다. 예산 파일 장애 시 영구 반복 보호와 원인 파일 장애 시 정확한 원인 통지는 제한된다. 최종 관련 pytest 98건·fixture 경계 10건·native fault_probe/handler/reset_cause 및 실기 18건이 통과했고 잔존 시험 프로세스 없이 원본·망·AP·서비스·boot ID·양 경로 ping을 최종 대조했다. 초기 실패·수정·최종 증거는 부모 저장소 `.omx/plans/opcd-fault-storage-20261013-hfkzjoyo/`와 testing-guide에 있다. 수정 정책은 로컬 staging에 있으며 타겟의 원본 정책/OPCd는 유지했다.
- **R3/R8 후속 결과 (2026-10-13)**: 새 0.7.0 DEB의 격리된 NXP OPCd와 현재 설치된 supplicant/checker/fw_watch를 사용했다. 각 SIGKILL 후 서비스 재시작 3.395/5.196/5.196초, fw_watch 초기 감시 대기 추가 60.279초와 원래 AP·OPC 응답·KeepAlive 복구를 확인했다. R8의 eth0 DOWN/UP에서는 carrier/IP/routes가 돌아와도 ARP·ping·유선 SSH와 외부 통지가 20초 안에 복구되지 않았다. PHY 재협상도 실패했고 정상 재부팅으로 수동 복구했다. 부팅 후 새 Ethernet MAC에서는 통신이 됐지만 이전 런타임 MAC으로 변경하면 다시 실패하여 부팅 MAC을 유지했다. 초기 peer_mac 캐시와 CHANGEADDR 갱신 부재가 MAC 변경 장애의 소스상 후보지만 설치 드라이버와 소스의 동일성 및 최초 DOWN/UP 실패 원인은 미확정이다. 원본 설정·계정·서비스를 보존하고 임시 자원을 정리했으며 운영 복구와 런타임 MAC 차이는 증거 보고서에 구분한다. HW watchdog의 실제 30초 설정은 읽기 전용으로 확인했으나 expiry/커널 정지를 주입하지 않았다.
- **진단·패키지 후속 결과 (2026-10-13)**: TERM을 무시한 진단 손자가 남는 결함을 별도 session의 그룹 KILL과 stdin FD 전달로 보완했다. helper 15건과 실제 checker/fw_watch 실행 회귀 4건이 통과했다. 격리 소스의 전체 release 빌드에서 pytest 1,339건·subtest 144건과 shell/schema/native·ARM64 NXP·DEB/source TAR gate가 통과했다. manifest에 이미 등록된 FTP 문서·QA 경로 5개를 TAR 검사에 정확히 허용했다. 새 DEB는 0.7.0/arm64이며 설치 없이 추출·payload 검증했다. 타겟의 새 monitor/policy 설치, 최소 이미지의 setsid 공급과 postinst 전체 운용은 미검증이다. 증거와 후보는 부모 저장소 `.omx/plans/opcd-r3-r8-package-20261013-omaneagb/`와 testing-guide에 있다.
- **범위**: Dual은 후속이며 외부 Release는 기존대로 보류한다. Ethernet 자동 복구 결함, 최종 NXP 패키지 설치·업그레이드, 실제 HW watchdog expiry·강제 리셋·자원 포화와 커널/저장장치의 물리적 교착은 별도 검증 범위다. GW/NTP의 ChangeIp 적용 범위는 이번 세 결정으로 변경하지 않는다. 아래 초기 분석의 미결 표기는 이 최신 방침과 후속 완료 기록을 우선하여 읽는다.

---

## ① 자율 구현 가능 (외부 대기 없음)

### 1.1 SetRadio mode/bw 실적용 · Single 검증 / Dual 운용·priority_ch 후속
- **현황(2026-10-13 갱신)**: NXP backend가 SCAN 주파수 목록과 mode/bw를 한 `opc_wlan_apply.sh` 호출로 전달한다. mode는 기존 Wi-Fi CLI와 같은 규격 상한(b/g/a/n/ac/ax), bw는 20/40/80MHz 상한이며, `bandcfg`/`htcapinfo`/`vhtcfg`로 적용한다. AP가 협상한 실제 규격/폭은 요청 상한보다 낮을 수 있다.
- **영속**: freq/SSID는 supplicant conf, mode/bw는 `wifi_init_conf.json`의 인터페이스별 `.radio`에 저장하여 기존 `wifi_init.sh` 부팅 경로가 재적용한다. OPC의 `radio.conf` 저장·ACK 정책은 유지하며 AP 재접속 완료는 기다리지 않는다.
- **검증/오류**: NXP는 드라이버 변경 전에 mlan1 ax·b/g+5GHz 전용 조합을 0x0013, 현재 helper가 구현하지 않는 160/80+80/320MHz를 0x0014로 거절한다. stub의 일반 프로토콜 enum 수용은 NXP 지원 범위를 보증하지 않는다. 적용 오류·일반 종료 신호는 드라이버 snapshot/JSON/conf 복구를 시도한다. 시간 초과 시 인터페이스별 900ms 안에서 그룹 TERM 정리 유예(150ms)와 그룹 KILL이 동작한다. SetRadioConfig는 저장 완료 후 응답하는 명령이므로 Rev1.01 §4.1.3.1의 120초 응답 타이머를 적용하며, Dual에 1초 예산을 나누지 않는다. 강제 종료 후 완전 복구는 보장하지 않으며 handler의 이전 설정 재적용도 best-effort다.
- **검증 환경 정정(2026-10-13)**: 이전 임시 QA 타이머의 `OnBootSec=25min`이 오래 켜진 타겟에서 이미 경과한 시각이어서 setup 중 복구 service를 즉시 실행했다. journal 실행 시각과 제어 소켓 추적으로 최초 mlan0 `DISCONNECT`가 검증 환경의 복구 실행과 겹쳤음을 확인했다. 이를 제품의 mlan1 기동 현상으로 해석하지 않는다. 타이머를 `OnActiveSec=25min` 단독으로 수정하고 매 관측 및 원복 직전 미발화를 확인했다. 이전 예비 시험의 원본 기록은 보존하며, 다음 새 결과를 통제된 실기 검증으로 사용한다.
- **실기 증거(2026-10-13, cts-wlan)**: mlan1 비활성 상태에서 mlan0를 `jhw_wlan`(5200MHz, 채널 40)에 연결했다. Single n/ac/ax × 20/40/80MHz 9조합에서 드라이버 상한·JSON·radio.conf 저장, 무선 ping 및 `GetDeviceInfo`를 대조했다(UDP ACK 462.666~538.679ms). n 요청은 HT20, ac/ax 요청은 VHT20으로 실제 TX와 OPC의 mode/BW가 일치했다. 지원하지 않는 BW 3건, b/g+5GHz 2건 및 Dual mlan1 ax 1건은 각각 0x0014/0x0013으로 사전 거절했고 mlan0 드라이버 상한·JSON/WPA/radio.conf 해시를 유지했다. 실제 ax·40/80MHz 링크와 재부팅 복원은 이번 재검증의 범위가 아니다.
- **Dual 실기/수정**: 기존 450ms 제한은 로컬의 인터페이스별 500ms helper 회귀에서 실패를 재현하여 900ms로 수정했다. 수정한 타이머 환경에서 `jhw_wlan_`(5180MHz, 채널 36) + `jhw_wlan`(5200MHz, 채널 40)에 n/20 + n/20을 요청하여 양 인터페이스 드라이버 상한·JSON 저장 및 OK ACK(1057.843ms)를 확인했다. 그러나 45초 관측에서 mlan0만 연결되고 mlan1은 SCANNING에 머물러 두 AP 동시 연결은 실패했다. 후속 Dual mode 조합과 60초 유지 시험은 실행하지 않았다. 앞선 AC/40 부팅·Dual 세 조합·mlan1 서비스 중단 복구 자료는 보존하지만 이번 타이머 미발화 재검증을 대체하지 않는다.
- **Single 재부팅·중단 추가 검증(2026-10-13)**: `jhw_wlan_`/5180MHz에서 ac/40을 저장한 뒤 재부팅 1회로 boot ID 변경, JSON/radio.conf 해시 유지, OPCd 시작 전 드라이버 상한 재적용과 무선 ping을 확인했다. 이 부팅 기록은 신호 마스크 수정 전 후보의 결과이며 기존 `wifi_init.sh` 부팅 경로는 변경하지 않았다. 이어진 중단 시험에서 OPCd의 signalfd용 SIGINT/SIGTERM 차단 마스크가 helper에 상속되는 결함을 확인했다. 자식의 exec 전에 마스크를 초기화하여 수정했고, 실제 차단 마스크의 부모에서 TERM 정리·부모 마스크 보존·TERM 무시 시 KILL을 회귀 검증했다. 전체 `make check`와 ARM64 NXP 빌드도 통과했다.
- **수정본 실기 복구**: 기준 ac/40·채널 36/40에서 n/20·채널 40만 적용하도록 요청하여 드라이버와 WPA 주파수 목록을 함께 바꿨다. reconfigure FAIL, TERM 지연, TERM 무시 후 KILL 3건 모두 NG/0x0050 응답(761.192/882.045/902.307ms) 후 기준 JSON/WPA/radio.conf 해시·드라이버 값, 원래 AP 연결 및 무선 ping이 복구됐다. helper의 SIGTERM 차단 비트는 해제되고 부모의 차단 비트는 유지됐다. TERM 지연은 KILL 없이 trap 정리가 관측됐고, TERM 무시 시험은 신호 수신 기록과 900ms KILL을 확인했다. FAIL 시험도 TERM 시각에 도달했으므로 순수 즉시 실패 경로라고 확대하지 않는다. KILL 뒤 남은 transaction 파일은 검증 도구가 inode/해시를 대조해 정리했으며 일반 동작의 완전 복구 보장을 추가한 것은 아니다.
- **이번 버전 범위**: 사용자 결정에 따라 Dual 및 `priority_ch` 추가 검증은 후속으로 두고 0.7.0은 Single 기준으로 진행한다. 최종 설치 패키지의 설치·업그레이드·재부팅, 실제 ax·40/80MHz 링크, 다른 보드/AP·장시간 운용은 별도 검증 범위다. 원본 파일·드라이버·MAC·IP·경로·서비스를 복원하고 모든 전용 원격 백업·타이머를 제거했다. 원본 증거는 부모 저장소 `.omx/plans/opcd-single-recovery-20261013/qa-summary.json`에 정리했다.
- **Dual 성공 시험 환경**: [NXP RN00104 Rev.20.0 §3.1.4](https://www.nxp.com/docs/en/release-note/RN00104.pdf#page=21)는 88W9098 CDW의 STA+STA를 5GHz+2.4GHz로 설명하고, [NXP 기술지원 답변](https://community.nxp.com/t5/Other-NXP-Products/88W9098-Dual-STA-Concurrency-Issue-2-4GHz-5GHz-Works-5GHz-5GHz/m-p/2321440)은 PCIe의 5GHz+5GHz 실패를 해당 제약으로 설명한다. 현재 SDIO 펌웨어의 상세 원인을 확정한 증거는 아니다. 타이머 수정 후 원래 mlan1 프로필로 2.4GHz passive scan을 완료했을 때 mlan0 연결은 유지됐고 AP 36개가 관측됐으나 `cantops`/`jhw_wlan`/`jhw_wlan_`은 없었다. 지원 조합의 성공 검증에는 사용할 2.4GHz AP의 SSID·채널·인증 정보 재사용 여부가 필요하다.
- **잔여**: Dual `priority_ch`의 데이터 경로 우선순위 의미는 미결이며 값 검증/저장/응답만 수행하고 실드라이버 steering은 구현하지 않는다. Dual의 양 AP 동시 연결·장시간 운용, 실제 ax·40/80MHz 링크 협상 및 다른 보드/AP는 미검증이다. 추가 시험 후에도 기존 파일의 SHA-256·소유권·모드, 양 인터페이스 드라이버·서비스·MAC·IP·경로를 복구하고 전용 임시 백업·복구 타이머를 제거했다. 외부 배포 승인은 보류 상태다.
- **파일/로컬 증거**: `opcd/platform_nxp.c`, `opcd/handler.c`, `opcd/tests/test_platform_nxp_radio.c`, 부모 저장소의 `opc_wlan_apply.sh` 및 `tests/test_opc_radio_apply.py`.

### 1.2 ChangeIp ESSID/Gateway/NTP apply — V3 · ~~essid M~~ 완료 / ~~ntp S~~ 완료(#161/#162, 실기 검증 대기)
- **현황(2026-06-17 갱신, #56 → wlan-package PR #49 반영)**: `nxp_apply_ip_change`는 IP/netmask를 `ip addr`(휘발)로 적용하고, **essid는 `run_opc_wlan_apply`(opc_wlan_apply.sh ssid → conf 직접편집 → `wpa_cli reconfigure`, 비휘발)로 적용 구현**(실타겟 검증, best-effort — 실패는 로그만, IP 결과 반환). **갱신(#161/#162)**: ntp는 `/run` timesyncd drop-in 휘발 적용 + `try-restart`, gateway는 `ip route replace default`(휘발, 0.0.0.0 제외), `nxp_get_ntp_server`는 drop-in 우선으로 읽는다. essid는 편집 직전에 복원 표식(`essid_marker.{c,h}`)을 쓰고(원래 `ssid=` 토큰·슬롯·시각·요청 SSID `target=` hex), 적용 성공 후 conf의 현재 `ssid=` 토큰을 `applied=`로 커밋하며, 실패·타임아웃 시에는 conf를 다시 읽어, 요청 SSID로 디코드되면 그 토큰을 커밋하고(helper가 설치한 뒤 실패), 다른 값이면 커밋하지 않으며(다른 작성자; 미커밋 표식 삭제·커밋 체인은 target 제거), 안 바뀌었으면 미커밋 표식을 지우고 커밋 체인은 실패한 요청의 target을 뺀다(`settle`). 잔여: SIGKILL 데드라인 뒤 runner가 helper 셸만 거두고 반환하므로 자식의 진행 중 rename(2)이 settle 직후 떨어질 수 있는 창(그 syscall 하나)은 닫지 않았다. 부팅 시 `opcd -R`은 conf가 커밋값이거나 `target=`으로 디코드될 때만(따옴표·hex 모두) 원복하고, 그 밖의 값이면 표식만 지우고 conf는 손대지 않는다(표식을 못 쓰면 ESSID 변경 자체를 하지 않음). 한계: 소유 판정이 값 기준이라 커밋값과 같은 SSID(따옴표·hex 어느 표기든)나 커밋·settle이 돌지 못한 요청의 SSID를 다른 작성자가 쓰면 구별이 안 돼 되돌아간다 — 그런 작성자(wifi.sh ssid/connect·프로비저닝·공장초기화)가 표식을 지우는 것과 `-R` 부팅 유닛(wifi_init.sh의 conf 변환·wpa_supplicant보다 먼저; -R은 conf writer lock을 잡지 않음)은 wlan-package#362가 추적한다(#361은 timesyncd 활성화·시험용 기본 경로 스크립트 제거만). 같은 IP를 다시 고르면 `ip addr replace`라 EEXIST 없이 GW·NTP가 적용되며(replace가 host-scope /32 미러에 성공해도 global 주소가 없으면 다른 주소를 지우지 않고 실패 반환), /32 슬롯의 off-block 게이트웨이는 `onlink`로 깔린다. 미구현(연기): `-R`이 IP·Mask·GW·NTP가 컨피그 값으로 돌아왔는지 로그로 남기기 — 부팅 시 `-R`은 네트워크 설정 전에 돌아 비교 대상이 없고, 재부팅 뒤 런타임 값은 어차피 사라지므로 #161 본문에서 범위를 다시 정한 뒤 처리.
- **파일**: `platform_nxp.c:875-940` (주석 886-890 "essid/ntp remain V3 on-target work (wifi_init.sh routing / wpa_supplicant)")
- **막힘**: essid = wpa_supplicant conf 재구성 시 활성 링크 드롭 정책 미정 / ntp = timesyncd.conf write+재시작 on-target 검증 대기
- **비고**: essid **읽기** 커널 fallback(device-info용, nl80211 GET_INTERFACE)은 2026-06-16 도입됐다가 **2026-09-21 #142에서 제거됨** — managed STA의 SSID는 인터페이스 상태가 아니어서 GET_INTERFACE 응답에 실리지 않으므로 그 fallback은 실행될 수 없었다(실기 확인: 연결 상태의 `iw mlan0 info`에 ssid 줄 없음, `wpa_cli status`에는 있음). **읽기 원천은 link.json의 `info.ssid` 단일**이며 로거가 wpa_supplicant에서 채운다(wlan-package #336). 여기 1.2 항목은 **쓰기**(change-ip가 SSID를 실제로 바꾸는 것)로 읽기와 별개다. set-ip-list의 essid 필드는 iplist.cfg에 저장되나 apply 시 무시됨.

### 1.3 Protocol Version 협상 + EEPROM identity — V7 · code-absent · **M**
- **현황**: codec이 protocol version을 read/write만, **버전 검증/협상 전무**. static identity(vendor/product/serial/hw)는 `device_info.json` 로드뿐 — **EEPROM/hostcmd 직접 읽기 미구현**.
- **파일**: `protocol/codec.c:37,59`, `platform_nxp.c:449-450`(주석 "canned" stale — 실로직은 inventory.c JSON), `inventory.c:119`
- **막힘**: 멀티버전 상호운용 요구 미확정 / EEPROM·hostcmd 인터페이스 미설계(현재 JSON config로 대체 운용)

### 1.4 AP_DISCONNECT 구분 / EHT 매핑 / 스텔스 빈SSID — code-absent · **M/S**
- **(a) Disassoc/Deauth 구분 불가**: nl80211 CMD_DISCONNECT에서 raw 802.11 프레임 미디코드 → 항상 Deauthentication(0x000C)으로 보고. `platform_nxp.c:1147-1169`
- **(b) EHT(802.11be) 모드 매핑 부재**: `parse_bitrate_to_mode`가 EHT-MCS tx_bitrate에 -EINVAL, OPC enum 없음(11ax까지만 매핑). `:138-152`
- **(c) 스텔스 빈SSID NULL 처리 미작성**(V4): 빈 SSID 분기 없음 (읽기 경로는 link.json `info.ssid` 단일 원천 — 1.2 비고 참조)

---

## ② 발주처/벤더/spec 대기 (자율 진행 불가)

| 항목 | 미구현 내용 | blocker | 파일 |
|---|---|---|---|
| **T9** 자율리셋 producer | 배관(emitter `opcd_ind_reset_notice` / consumer / `OPCD_PEVT_RESET_NOTICE` enum)은 완비, **자율 트리거 코드·cause ID 부재**(user reset만 존재) | #35 — 트리거 조건(watchdog/fault)·자율 cause 값 미정 | `platform_nxp.c:1218`, `indication.c:81`, `opcd.c:112`, `ids.h:139` |
| **A5** 무효문자 검증 | SetPassword/Login/ESSID 모두 NUL종단만 검사, **문자집합(무효문자) 검증 없음**(0x0015 UNUSED) | #35 — '무효문자' 정의가 사양에 부재 | `handler.c:591-632`(SetPassword), `:350-404`(Login) |
| **wlan_id** Dual 구분 | indication에 wlan_id 필드 없어 **mlan1(idx≠0) 이벤트 드롭** → Dual WLAN#1/#2 구분 불가(잠정 mlan0) | #35 — dual 발행 정책 + 실 dual-radio HW | `opcd.c` on_platform_event |
| Memory(0x0002) | swapless 타깃이라 사양 정의(페이징) 성립 불가 → **의도적 미발행**(Disk I/O로 일원화) | 확정(코드 변경 불요) | `fault_probe.c` |
| capability 비트 | `device_info.json` 정적값(전부 1=추정) vs silicon 실광고 미검증 | #35 — 고객사 문의 | `inventory.c` |
| 0x0018 / gateway | 0x0018(비정상 boundary NG) 벤더 확정 대기 / gateway는 #27의 미적용 결정이 #162(2026-10-17)로 **대체되어 휘발 적용** | #35(A17) / #27(확정) | `handler.c`, `platform_nxp.c:886-889` |
| ~~**G11** device-info FREQ/CH~~ | **Rev1.01 §4.3.4로 확정(2026-08-06): 접속 시 주파수(접속값), 미접속 0xFFFF** → #103에서 출하 기본 `live`로 전환, `config`(Rev1.00 해석)·`auto`는 옵션 유지 | 종결 (#103, 2026-09-04) | `freq_source.{c,h}`, `handler.c`(select_devinfo_freq_ch) |

---

## ③ 참고 — 구현은 됨, "실측만" 남음 (미구현 아님)
- **V1** Roaming/ApDisconnect (#47 — WlanStatusChange는 2026-06-16 와이어 캡처 검증)
- **V10** NVRAM 120s fsync 예산(i.MX8MM eMMC) · **V11 power-cycle IP 복원 (2026-06-16 reboot로 검증됨)**
- **V13~V17** (규제 NG 실동작 / ESSID 길이 경계 / mlan0 라우팅 소유 / DUAL 검출 / idle 경계)
- `nxp_prepare_reset`은 #164로 `wlan_reboot_policy.sh --source opc_reset --reason opc_reset_command --force`를 20 s 상한으로 호출(Ack → ResetNotice → 호출 순서; SIGTERM 종료·상한 초과 = 재부팅 진행 중, dedupe 충돌 = 다른 요청이 락 보유(미확인) → 0; exit≠0(실행기 기동 실패·중단)·126/127(실행 불가)·SIGTERM 아닌 시그널 종료 = 재부팅 없음 → systemd 재기동 폴백) · **V8** IP-only 세션 식별은 운영정책(SEC-001, 코드 범위 밖)

---

## ④ 사양 Rev1.01(2026-08-06) 정합 — 이슈 #101~#107 (2026-09-03 등록)

| 이슈 | 항목 | 상태 |
|---|---|---|
| #101 | GetDeviceInfo 응답 WLAN#1/#2 블록 오프셋 정정(프레임 284/344 → 288/352) + SCAN Frequency Band·SCAN Channel List 필드 추가(미설정값 0xFFFF / ALL 0 에코) | **완료** — `test_codec` 절대 오프셋 단언, `fielddump`·스냅샷·vhlctl 동기 |
| #102 | SetRadioConfig 요구 레이아웃 재정의(Length 76→84, SCAN Band + 64bit Channel List) | **완료** — `protocol/scan_chlist.{c,h}` 비트맵 헬퍼(워드 순서 가정 1곳 격리·관용 디코드), 검증 0x0011/0x0012 재정의, 비트맵→MHz 목록 적용, 동일 설정 재전송 시 apply 생략, radio.conf 옛 16B 포맷 변환, vhlctl `--w1-band/--w1-chlist` |
| #103 | 미설정·미접속·무효값 규약(0xFFFF/0xFF/-128/ALL 0) + `device_info_freq_source` 기본값 | **완료** — 미접속 FREQ/CH 0xFFFF·SNR/RSSI -128·AP MAC 0·Status 0, 미설정 Mode/BW 0xFF, Single Priority CH 0xFFFF + WLAN#2 블록 전체 무효값; `device_info_freq_source` 기본 **live**(enum 0), `config`는 레거시 옵션. 테스트 25/28, `test_freq_source` |
| #104 | 재송(Retry) 응답 SN: 재송 폐기·원 요구 SN 응답 | **완료** — SET_RADIO 재송(동일 payload + ack 대기 중)을 apply 전에 폐기, 원 요구 SN pending ack 유지(그림 4-2). 재적용·2차 write 없음. 엇갈림(cross) 재송은 응답 후 도착 → 자기 SN으로 즉시 응답. 재송 판정은 전체 payload를 해당 (ip,port) pending 슬롯이 보관한 요청과 비교(전역 st->radio 아님). 테스트 21/21b/21c/21d. password/ip 재송의 원-SN 응답 확장은 Q6 회신 후 후속 |
| #105 | Indication Period 내 마지막 상태만 통지(coalesce) | **완료** — WlanStatus/Roaming/ApDisconnect는 period>0이면 종류별 슬롯에 최신 상태만 적재하고 period 경계(opcd_ind_tick)에서 1건 flush(에지→주기 샘플링). period=0은 즉시 송신 유지(발주처 Q). 설정변경 시 슬롯 정리(로그아웃 시 정리는 #160으로 제거 — 통지 설정이 세션과 무관해져 대기 중 상태 변화를 버리지 않음). 슬롯 키에 링크 idx(#106 전방호환, 현재 idx0만). 테스트 23b/23c |
| #106 | Dual 링크별 통지 — 링크 식별 방식 발주처 문의 선행 | 차단 |
| #107 | SetIPConfigList Boundary 0x0003(시작 및 완료) 단일 프레임 커밋 | **완료** — 0x0003 = START(확정 리스트 시드·병합) + END(커밋·iplist.cfg 저장) 단일 프레임. 0x0003 뒤 CONTINUE/END는 START 없는 시퀀스 → 0x0018. 테스트 14d/14e, vhlctl `--flag start_end` |

## ⑤ Rev1.01 문의(Q1~Q10) 검토 후속 — 이슈 #120~#122 (2026-09-04 등록)

| 이슈 | 항목 | 상태 |
|---|---|---|
| #120 | SetPassword/SetIPConfigList 재송을 §4.1.3(재송 폐기·원 요구 SN 응답)으로 정렬 + 낡은 주석·테스트 정합 | **완료** (PR #124) — pending ack 슬롯에 요청 body 보관, 명령 공통 `is_body_retransmission`(같은 (ip,port) in-flight 슬롯과 memcmp) → 재송 폐기·원 SN 응답. 주석 정정(A19/Q6/period-0/NTP 0.0.0.0). 테스트 21e~21l·14i 0.0.0.0·14d-2 로그인 게이트 |
| #121 | 장애 검출 통지(0x0010) 측정 타이머를 Indication Period에서 분리 + 폭주 진입 시 1회 통지 | **완료** — probe는 `opc.conf::congestion_probe_interval_s`(기본 10 s, 1..3600) 자체 카운트다운으로 1 s tick에서 분리(Period 0에서도 동작). 자원별 진입 래치: 진입 전이만 통지(Period 0 즉시, ≥1은 #105 슬롯 staging 후 주기 말 1회), 지속 시 재통지 없음, 해소는 훅만(Q6 대기). **해제는 `congestion_clear_pct`(기본 = 진입 임계 − 10, 즉 70%) 아래로 떨어질 때** — 진입/해제 임계가 같던 #141 이전에는 임계 근처 진동(79/81/79…)이 매 표본마다 새 진입으로 통지됐다. 밴드 상태와 통지 래치는 별도 플래그이며, 재무장/래치 초기화는 **통지 래치만** 지운다(밴드 안에 머무는 폭주를 새 수신자·재시도에 다시 알리기 위함). SetIndicationConfig 시 래치 초기화(새 수신자에 진행 중 폭주 1회 통지; 로그아웃 시 초기화는 #160으로 제거). 테스트 23·test_fault_probe 8/9 |
| #122 | peer_route 구성에서 ChangeIp·device_ip_iface·서브넷 겹침 가드 검증 및 정합 | **완료** (PR #127, 실기 검증 2026-09-04 cts-wlan peer-route 프로파일: ChangeIp .100→.101→.100 왕복에서 mlan0·eth0 /32 미러(첫 주소)·t100 갱신 확인, 이슈 코멘트에 기록) — 토폴로지 판정 `opcd/topology.{c,h}`: 소스는 `wifi_init_conf.json` `.wbridge.peer_route.enabled`(wifi_init.sh와 동일 토글), opc.conf `management_topology = auto\|eth0_ip\|peer_route`는 테스트용 override. peer_route면 IP 읽기/적용 평면을 mlan0로 강제(device_ip_iface 무시·경고), ChangeIp 성공 후 플랫폼 훅 `peer_route_refresh`로 eth0 /32 미러·table 100 link route·peer host route src를 새 IP로 갱신(wifi_init.sh 시퀀스 재현, 스크립트는 20-mlan0.network를 읽어 휘발성 ChangeIp와 맞지 않음). 겹침 가드는 변경 없음(/32는 서브넷 충돌이 될 수 없음). 테스트 28·test_topology. |

## ⑥ V1 후속 — 이슈 #47 (2026-06-15 등록, 2026-09-04 착수)

| 항목 | 상태 |
|---|---|
| ② ASSOCIATED 채널 보강 | **완료(PR #49)** — CONNECT WIPHY_FREQ 우선, 없으면 커널 GET_INTERFACE 조회 |
| ③ T9 자율 리셋 ResetNotice producer | **완료** — `opcd/reset_cause.{c,h}`: SIGTERM 시 `systemctl is-system-running`=stopping이면 `/run/opc/reset_cause`(없으면 0x0002 SYSTEM)로 ResetNotice 1회. Reset Cause 표 `protocol/ids.h`. 부모 저장소 `wlan_reboot_policy.sh`의 cause 파일 기록은 후속(없어도 동작) |
| ① ApDisconnect MAC(AP 주도) 실측 | **보류** — 로컬 disconnect는 ApDisconnect 미발행(WlanStatus만). AP 측 deauth 수단(실 AP 접근 또는 mlan1 AP 모드) 확보 후 별도 이슈 |

## 우선순위 요약
- **임팩트 순(자율)**: ① SetRadio 실반영(L) > ② ChangeIp essid/ntp(M/S) > ③ Version/EEPROM(M) > ④ AP구분/EHT/스텔스(M/S)
- **②는 #35 발주처 답변** 의존 — 답변 수신 시 T9 producer·A5 검증·wlan_id는 배관 완비라 소규모 추가로 마무리 가능
- 코드 스냅샷 기준 라인은 후속 커밋으로 drift 가능 — 착수 시 재확인
