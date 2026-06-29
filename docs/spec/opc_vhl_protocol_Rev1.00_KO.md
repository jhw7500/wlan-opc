# 무선 기판 공통 제어 통신 사양서 (無線基板共通制御通信仕様書)

> 원본: `無線基板共通制御通信仕様書_Rev1.00_CANTOPS送付用 (1)-260525.pdf` (전 41페이지)
> 일본어 → 한국어 번역본. 기술 용어 일부는 원문 표기(VHL, OPC 기판, Login 등)를 병기/유지함.
> 레지스터/바이트맵 도면은 코드블록으로 재구성(원본은 32비트 폭, 바이트 오프셋은 왼쪽 숫자).
> [용어 일러두기 2026-06-11] 원본(한국어 docx)의 "기판"(基板 — Single/Dual Station 기판,
> 기판 정보 취득, 기판 기동 중 등)을 본 번역본에서는 계통적으로 "장치"로 치환했다. 동일 대상을
> 가리키며 와이어 포맷에는 영향이 없으나, 원본과의 용어 대조 시 이 치환을 감안할 것.
> 2026-06-11 원본 docx(텍스트+도면 33매) 전수 교차검증을 수행해 전사 오류를 정정했다 —
> 본문 내 "[정정/보완 2026-06-11]" 노트 참조.

- **Rev.** 1.00
- **일자** 2026.05.25
- **작성** Ochiai Tsuneo / 落合 庸央 (개정이력 기재자: 平尾 / 히라오)

---

## 개정 이력

| 판수 | 일자 | 기재 위치/내용 | 비고 |
|---|---|---|---|
| 1.00 | 2026.05.25 | 초판 발행 | 平尾(히라오) |

## 관련 문서

| 문서명 | 판수 | 일자 | 비고 |
|---|---|---|---|
| (해당 없음) | | | |

---

## 목차

1. [들어가며](#1-들어가며) … 5
2. [무선 기판의 장치 종별](#2-무선-기판의-장치-종별) … 5
   - 2.1 Single Station 장치 … 5
   - 2.2 Dual Station 장치 … 5
3. [UDP/IP 무선 기판 제어 통신 사양](#3-udpip-무선-기판-제어-통신-사양) … 6
   - 3.1 통신 사양 … 6
     - 3.1.1 Interface 및 Protocol … 6
     - 3.1.2 포트 번호 … 6
     - 3.1.3 UDP 통신 방식 … 6
   - 3.2 통신 포맷 … 9
     - 3.2.1 공통 헤더 … 9
   - 3.3 Request/Query 형 커맨드 … 10
     - 3.3.1 Login 설정 … 11
     - 3.3.2 Logout 처리 … 12
     - 3.3.3 기본 정보 취득 처리 … 13
     - 3.3.4 장치 정보 취득 … 14
     - 3.3.5 패스워드 설정 … 19
     - 3.3.6 IP 주소 리스트 설정 … 21
     - 3.3.7 IP 주소 변경 … 26
     - 3.3.8 무선 설정 변경 … 28
     - 3.3.9 Indication 형 메시지 통지 설정 … 31
     - 3.3.10 리셋 요구 … 33
   - 3.4 Indication 형 메시지 … 34
     - 3.4.1 장치 초기 설정 완료 통지 … 35
     - 3.4.2 무선 접속 상태 변화 통지 … 36
     - 3.4.3 로밍 통지 … 37
     - 3.4.4 AP로부터의 절단 수신 통지 … 38
     - 3.4.5 장치 장애 검출 통지(무선 기판 이상 검출 시) … 39
     - 3.4.6 장치 리셋 통지 … 40
     - 3.4.7 Keep Alive 통지 … 41

**그림 목차**
- 그림 2-1 Single Station 장치 … 5
- 그림 2-2 Dual Station 장치 … 5
- 그림 3-1 Request/Query 형 시퀀스 … 7
- 그림 3-2 Request/Query 메시지 송신 시 재송 처리 이미지 … 7
- 그림 3-3 Indication 형 시퀀스 … 8
- 그림 3-4 통신 포맷 … 9
- 그림 3-5 IP 주소 리스트의 컨피그레이션 파일 구성 … 21
- 그림 3-6 IP 주소 리스트의 불휘발 저장 시퀀스 이미지 … 23
- 그림 3-7 IP 주소 변경 처리 이미지 … 26

**표 목차**
- 표 3-1 Request/Query 형 커맨드 Request ID 일람 … 10
- 표 3-2 WLAN Mode 설정값 … 17
- 표 3-3 주파수 대역폭 설정값 … 18
- 표 3-4 Indication 형 메시지 ID 일람 … 34

---

## 1. 들어가며

본서는, VHL에 탑재하는 무선 기판을 멀티 벤더화하기 위해, VHL과 무선 기판 간의 공통 제어를 수행하기 위한 통신 사양에 대해 기술한 것이다.

통신 사양에 관해서는, UDP/IP 패킷에 의한 무선 기판 제어 사양서로서 규정한다.

## 2. 무선 기판의 장치 종별

본서에서는, VHL에 탑재하는 무선 기판에 대해, **Single Station 장치**와 **Dual Station 장치**라는 2종류의 무선 기판 장치를 정의한다.

### 2.1 Single Station 장치

Single Station 장치는, 무선 송수신기를 하나만 보유하며, 1개의 주파수대 채널로 AP와 접속하여 데이터 통신을 수행하는 무선 기판으로 한다.

```
[VHL(OPC 기판)] --- Ethernet --- [무선 기판] ~~~ 5180MHz   (그림 2-1)
```

> [정정 2026-06-11] 초판 재구성은 "(OPC 기판)"을 무선 기판 쪽에 붙였으나, 원본 도형·본문
> (§3.2.1 "VHL(OPC 기판)이 패킷 송신마다…" 등 9곳) 모두 **(OPC 기판)은 VHL 측 호칭**이다.
> 또한 원본 그림 2-1은 1채널(5180MHz), 그림 2-2는 2채널 동시 접속(5180MHz+2124MHz)을 표기한다.

### 2.2 Dual Station 장치

Dual Station 장치는, 무선 송수신기를 복수 보유하며, 동시에 2개의 주파수대 채널로 AP와 접속하여 데이터 통신을 수행하는 무선 기판으로 한다.

```
[VHL(OPC 기판)] --- Ethernet --- [무선 기판] ~~~ 5180MHz + 2124MHz (2채널 동시)   (그림 2-2)
```

---

## 3. UDP/IP 무선 기판 제어 통신 사양

### 3.1 통신 사양

#### 3.1.1 Interface 및 Protocol

VHL과 무선 기판은, 기본적으로 운용상 유선 LAN(Ethernet)을 사용하여 통신한다. Protocol로는 UDP/IP를 사용한다.

#### 3.1.2 포트 번호

제어용 통신에 사용되는 포트 번호는, 무선 기판의 Config 데이터로 지정 가능하도록 한다.
기본(default) 포트 번호에 대해서는 **(TBD)** 향후 결정한다.

#### 3.1.3 UDP 통신 방식

통신 방법으로, **Request/Query 형**과 **Indication 형**의 2가지 방식을 지원한다.

##### 3.1.3.1 Request/Query (요구·응답형)

Request/Query 형(요구·응답형)은, 통신 주체로서 VHL로부터의 명시적 요구에 대해 무선 기판으로부터의 응답을 기다리는 **동기형**이다. 정보 취득이나 처리 실행 의뢰, 설정 변경 시에 사용한다.

본 커맨드 처리는, 응답 대기 타이머를 설정하고 대기 처리를 요구 측에서 수행한다. 대기 타이머 값으로는 다음 2종류를 정의한다.

1. 참조계 커맨드 및 불휘발 메모리에 저장이 필요 없는 커맨드: **1초**
2. 설정계 커맨드로서 불휘발 메모리에 저장이 필요한 커맨드: **2분**

본 규정의 응답 시간에 맞추지 못할 우려가 있는 경우에는, 최대 응답 시간을 무선기 형식별로 벤더가 제시하도록 한다.

**그림 3-1 Request/Query 형 시퀀스**
- (VHL) 정보 취득·처리 실행·설정 변경 요구 → 요구 메시지 송신 후 응답 대기 타이머를 설정하고, 응답 메시지가 수신되지 않으면 타임아웃으로 요구를 무효로 한다.
- (무선 기판) 불휘발 메모리 저장이 있는 경우, 불휘발 저장 후에 응답 메시지를 송신한다.

제어 요구 메시지의 공통 헤더에서 지정된 시퀀스 번호가, 응답 메시지의 시퀀스 번호로 설정되어 반신된다. 이로써 프레임 중복이나 응답 식별이 가능해진다.

설정계 요구 커맨드로 불휘발 메모리 저장 처리가 있는 경우, 응답 메시지는 불휘발 메모리에 저장한 후 송신할 것.

VHL로부터의 재송 처리로 동일 처리의 제어 요구가 있었던 경우, 무선 기판이 처리 실행 중이면 재송 처리로서 요구를 폐기하고, 처리 완료 후에 응답 처리를 수행한다. 응답 처리 송신 후에 엇갈림으로 재송 처리를 접수한 경우에는, 요구 처리 실행 후에 응답 메시지를 응답한다.

**그림 3-2 Request/Query 메시지 송신 시 재송 처리 이미지**
- 무선 기판이 SN=11 처리 실행 중일 때, 요구 측 타임아웃 재송은 **새 SN=12를 달고 도착**(§3.2.1 "패킷 송신마다 번호 가산") → 무선 기판은 동일 요구로 판단해 **대기 중이던 SN=11(에 대한 응답)을 폐기**하고, 처리 완료 후 **SN=12로 응답**.
- 응답과의 엇갈림으로 재송 요구(SN=13)를 수신한 경우, 신규 요구로 수신하여 처리 실행 후 응답. (요구 측은 응답을 2연속 수신 처리)

> [정정 2026-06-11] 초판 재구성 "동일 요구 수신(SN=11)은 폐기"는 원본 도면의 "(SN=11 폐기)" 괄호
> 경계를 옮겨 적은 것으로, 재송 패킷 자체가 SN=11로 도착하는 듯 읽혔다. 원본 도면(그림 3-2)의
> 재송 화살표는 **제어요구(SN=12)** 이며, 중복 판단 기준은 'SN 동일성'이 아니라 **동일 커맨드의
> 새 요구 도착**이다.

##### 3.1.3.2 Indication 형 (통지형)

Indication 형(통지형)은, Indication 통지 설정(통지처 주소와 통지처 포트 번호)이 되어 있는 경우에, 무선 기판의 상태 변화(무선 LAN 측 절단·접속·로밍 관련·초기화 완료 등) 검출에 의해 **비동기로 통지**된다. 일정 주기로 정보 통지를 시킴으로써, 무선 기판의 Keep Alive로도 사용 가능하게 한다. 통지 주기 통지 유무, 정의 사상(事象) 변화 시의 통지 유무를 설정 가능하도록 한다.

Indication 형은, Query 형 메시지(장치 상태 취득) 대신 사용하는 것을 목적으로 하며, Indication 형의 통지 설정을 유효로 한 후에는 Query 형에 의한 장치 정보 취득은 무효가 된다.

**그림 3-3 Indication 형 시퀀스**
- (무선 기판) 상태 변화 검출, 통지처 지정이 있는 경우에 통지. 주기 보고값이 설정된 경우 주기마다 사상 변화를 체크하여 무선 LAN 측 정보를 통지한다. 통지마다 Sequence Number 갱신을 무선 기판에서 수행한다.

---

### 3.2 통신 포맷

VHL ― 무선 기판 간에 사용하는 통신 포맷의 기본 구성을 아래에 나타낸다. **바이트 오더는 빅엔디안**으로 한다.

```
오프셋  ┌─────────────────────────────────────────────────────────────┐
        │ bit  7..0     7..0          7..0 ............ 7..0            │
  0     │ Protocol Ver │ Command Type │ Request/Indication ID (2B)     │  ┐
  4     │ Sequence Number (2B)        │ Length (2B)                    │  │ 공통 헤더
  8     │                                                             │  │ (64B)
  …     │                 Reserve                                      │  │
 60     │                                                             │  ┘
 64     │ Command Payload                                              │  ┐ 커맨드 페이로드부
        │   Command / Response / Report Area                           │  │ (Command/Response/
 …      │                                                             │  │  Report Area)
1420    │                                                             │  ┘
        └─────────────────────────────────────────────────────────────┘
  UDP/IP 페이로드 = 최대 1424 Byte
  (그림 3-4 통신 포맷)
```

> ✅ **구현 정합 (원본 docx 도면으로 확정)**: 위 도면이 정확하다 — 원본 docx의 바이트맵 도면(그림 3-6 + 커맨드별 포맷)으로 교차확인됨. 구현(`protocol/proto.h`)도 이에 맞춰 정렬됨:
> - **공통 헤더 = 64바이트** = 고정 8B(Protocol Version·Command Type·Req/Ind ID·Sequence·Length) + **Reserve 56B(bytes 8..63)**. **Command Payload는 offset 64부터** 시작.
> - **Length = (전체 프레임 − 8)** = Reserve(56) + payload. 예) Login 요구 payload 128 → 프레임 64+128=192 → **Length=184**.
> - **최대 프레임/UDP = 1424B** (= 헤더 64 + payload 1360), **Length 최대 = 1416B** (= 1424 − 8), payload 최대 1360B.
> - **빈요구(Logout / 기본정보취득 / 장치정보취득 / 리셋)는 8바이트 고정 헤더만** 송신(Reserve 없음) → `Length = 8 − 8 = 0` (예외 아님). 아래 3.3.10 리셋 **응답**의 `Length=0`만 오타로 판단해 `Length=60` 채택.
> - 근거·결정 이력은 `docs/spec-inquiry/proto-todo.md` T3·T10·T11·T12 참조 (한때 60바이트/`−4`로 구현했다가 도면 확인 후 64바이트/`−8`로 정정).

#### 3.2.1 공통 헤더

- **Protocol Version** — 무선 통신 기판 제어 프로토콜의 포맷 버전 식별자. 리퀘스트 시에는 요구 측이 지원하는 최고 프로토콜 버전을 넣는다. 응답 측은 구현된 프로토콜 버전을 넣어 응답한다.
- **Command Type** — 통신 제어의 커맨드 타입. `Request(=0x01)`, `Acknowledgment(=0x02)`, `Indication(=0x03)`를 설정한다.
- **Request/Indication ID** — Request 및 Indication 커맨드의 종별. Acknowledge는 Request와 동일한 ID가 설정된다.
- **Sequence Number** — 프레임 중복 방지 및 재송 판단을 위한 카운터.
  - Request·Query 메시지 송신 시, VHL(OPC 기판)이 패킷 송신마다 번호를 가산한다. 무선 기판 측은 Request·Query에 대응한 Acknowledgment 송신 시, Request·Query에서 수신한 Sequence 번호를 복사하여 반신한다. VHL(OPC 기판) 측은 수신한 Acknowledge 메시지가 어떤 Request·Query에 대한 응답인지 판단한다.
  - Indication 메시지 송신 시, 무선 기판 측이 통지하는 메시지마다 번호를 가산한다. 수신 측(VHL)은 Sequence 번호를 체크하여 중복 통지를 판단한다.
- **Length** — Request/Indication/Acknowledge의 페이로드 길이. (최대 1416 byte)

---

### 3.3 Request/Query 형 커맨드

Request/Query 형 커맨드에는, 무선 기판에 대해 정보 취득을 수행하는 요구와, 설정 처리를 수행하는 커맨드가 존재한다. 설정 처리 커맨드에 관해서는, 설정 요구의 배타 제어나 보안(잘못된 설정)을 고려하여, **Login·Logout 요구 기능**을 사용해 보안과 배타 제어(복수 동시 요구 억제)를 실시한다.

본 장에서는, Request/Query 형 커맨드와 그 응답에 관한 포맷의 상세를 기재한다.

#### 표 3-1 Request/Query 형 커맨드 Request ID 일람

| 구분 | 커맨드 | Login 요부 | Request ID |
|---|---|---|---|
| Login/Logout (보안/배타 제어) | **Login**: 설정·참조계 커맨드 투입을 가능하게 하는 Login 처리. 복수 요구원의 경합 제어(배타 제어). | — | `0xF001` |
| | **Logout**: 설정·참조계 커맨드 투입 권한 해방(Logout 처리). | — | `0xF002` |
| 정보 취득계 | **Get Basic Information**: 기본 정보 취득 | 불요 | `0x0001` |
| | **Get Device Information**: 장치 정보 취득 | 요 | `0x0002` |
| 설정 처리계 | **Set Password**: 패스워드 설정 | 요 | `0x1001` |
| | **Set IP Config List**: IP 주소·ESSID 등 변경 리스트 설정 | 요 | `0x1002` |
| | **Change IP Address**: 리셋 없이, 지정된 IP 주소 번호의 IP 주소·ESSID 등으로 변경 | 요 | `0x1003` |
| | **Set Radio Config**: 무선 설정 변경 | 요 | `0x1004` |
| | **Set Indication Config**: Indication 형 메시지 수신 설정 | 요 | `0x1005` |
| | **Reset**: 무선 기판 리셋 요구 | 요 | `0x2001` |

---

#### 3.3.1 Login 설정

무선 장치 설정을 위해, Login 처리를 수행하여 설정·참조 권한을 취득한다. Login 처리는 Logout 누락 방지를 위해, Login 상태에서 설정·참조 커맨드를 **5분 이상 수신하지 않으면 자동으로 Logout 처리**한다.

- **발행 조건**: Login 상태가 아닐 것. 이미 다른 IP 주소 장치로부터 Login을 접수한 경우에는, 배타 제어를 위해 에러 응답한다. Login 상태에서는 모든 설정·참조 처리가 가능하다.

**요구 (VHL→무선 기판) 포맷**
```
  0  Protocol Version=1 | Command Type=1 | Request/Indication ID=0xF001
  4  Sequence Number=0xXXXX | Length=184
  8  ┐
  …  │ Reserve
 60  ┘
 64  password : 패스워드 문자열 (최대 127자)  ex "MyPassword"
 …
188
```
- ◆ **password**: 패스워드 문자열 (최대 127자, NULL 종단)

**응답 (무선 기판→VHL) 포맷**
```
  0  Protocol Version=1 | Command Type=2 | Request/Indication ID=0xF001
  4  Sequence Number=0xXXXX | Length=60
  8  ┐
  …  │ Reserve
 60  ┘
 64  Result | Error Cause
```
- ◆ **Result**: 결과 OK(=0x0000) / NG(=0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000): Result가 OK인 경우
  - Login 발행 위반(0x0001): 장치 기동 중 상태에서 발행한 경우
  - Login 조건 위반(0x0002): Login 상태에서 2중 발행(다른 IP 주소에서 Login되어 있는 경우)
  - 패킷 사이즈 지정 오류(0x0003): 공통 헤더 Length가 잘못된 경우
  - 패스워드 오류(0x0010)
  - 패스워드 무효 문자 지정(0x0011)
  - 패스워드 NULL 종단 위반(0x0012)

---

#### 3.3.2 Logout 처리

설정·참조 권한 해방을 위해 Logout 처리를 수행한다.

- **발행 조건**: Login 상태이며, Login 처리한 장치와 동일한 IP 주소가 송신원일 것. Login 상태가 아니거나, Login 요구와 다른 IP 주소로부터의 Logout 처리는 에러 응답된다.

**요구 (VHL→무선 기판) 포맷**
```
  0  Protocol Version=1 | Command Type=1 | Request/Indication ID=0xF002
  4  Sequence Number=0xXXXX | Length=0
     파라미터 없이, 공통 헤더만으로 커맨드 송신.
```

**응답 (무선 기판→VHL) 포맷**
```
  0  Protocol Version=1 | Command Type=2 | Request/Indication ID=0xF002
  4  Sequence Number=0xXXXX | Length=60
  8  ┐ Reserve
 60  ┘
 64  Result | Error Cause
```
- ◆ **Result**: OK(=0x0000) / NG(=0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000): Result가 OK인 경우
  - Login 발행 에러(0x0001): Login 커맨드가 발행되지 않은 상태에서 발행
  - Login 조건 에러(0x0002): Login 커맨드를 발행한 IP 주소와 다른 IP 주소로부터의 요구 수신
  - 패킷 사이즈 지정 오류(0x0003): 공통 헤더 Length가 잘못된 경우

---

#### 3.3.3 기본 정보 취득 처리

장치 벤더나 장치 종별 등의 기본 정보를 취득한다.

- **발행 조건**: 없음. (전원 ON 또는 리셋 후, Login 처리 없이 발행 가능) 프로토콜 버전이 달라도 본 커맨드의 요구 포맷은 변경되지 않는다. 본 요구는 발행 조건이 없으므로 반드시 응답한다.

**요구 (VHL→무선 기판) 포맷**
```
  0  Protocol Version=1 | Command Type=1 | Request/Indication ID=0x0001
  4  Sequence Number=XXXXX | Length=0
     파라미터 없이, 공통 헤더만으로 커맨드 송신.
```

**응답 (무선 기판→VHL) 포맷**
```
  0  Protocol Version=1 | Command Type=2 | Request/Indication ID=0x0001
  4  Sequence Number=XXXXX | Length=72
  8  ┐
  …  │ Reserve
 60  ┘
 64  Vendor Code (4Byte)
 68  Product Code (2Byte) | Product Subcode (2Byte)
 72  Device Status (4Byte)
 76  Reserve | Station Type (2Byte)
```
- ◆ **Vendor Code**: IEEE 등록 OUI(3byte) 값. 예) CONTEC사(0x0000804C), CANTOPS사(0x00902CFB) 등
- ◆ **Product Code**(장치 형식): 장치 벤더가 정의하는 장치 종별 코드. FXE3000, FXE5000 등의 장치 형식을 나타냄
- ◆ **Product Sub Code**(서브 장치 형식): 장치 벤더가 정의하는 서브 코드. 동일 형식에서 국가별 대응 장치 종별을 나타냄 (일본, 중국, EU, 북미 등의 종별 코드)
- ◆ **Device Status**: 장치 상태 표시
  - 장치 기동 중 상태: 0x00000000 (Login 커맨드 요구나 기타 설정 처리 불가 상태)
  - 장치 기동 완료: 0x00000001 (Login 커맨드 수신 대기 상태)
  - Login 중 상태: 0x00000002 (Login 커맨드 수신 완료 상태)
- ◆ **Station Type**: 스테이션 타입
  - Single Station=0x0001: 1주파수로 접속하는 단말
  - Dual Station=0x0002: 2주파수로 접속하는 단말

---

#### 3.3.4 장치 정보 취득

장치 정보를 취득한다.

- **발행 조건**:
  1. Login 되어 있을 것.
  2. Indication 형으로 장치 정보 통지가 유효한 경우, 본 커맨드에 의한 상태 조회는 금지(무효) 상태가 된다.

**요구 (VHL→무선 기판) 포맷**
```
  0  Protocol Version=1 | Command Type=1 | Request/Indication ID=0x0002
  4  Sequence Number=XXXXX | Length=0
     파라미터 없이, 공통 헤더만으로 커맨드 송신.
```

**응답 (무선 기판→VHL) 포맷** (Length=408)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x0002
  4   Sequence Number=XXXXX | Length=408
  8   ┐ 공통 헤더
  …   │ Reserve
 60   ┘
 64   Result | Error Cause
 68   Vendor Code (4Byte)
 72   Product Code (2Byte) | Product Subcode (2Byte)
 76   Date of Manufacture (4Byte → Year 2byte, Month 1byte, Day 1byte)
 80   Date of Shipment    (4Byte → Year 2byte, Month 1byte, Day 1byte)
 84   Firmware Version (최대 31자 NULL 종단)  ex "XXX"
 …
112
116   Hardware Version (최대 31자 NULL 종단)  ex "XXXXXXXX"
 …
144
148   Serial number OR Device-specific number (최대 31자 NULL 종단)
 …
176
180   Ethernet MAC Address (6Byte)  ← 장치 정보
184   Reserve
188   IP Address (4Byte)
192   SubnetMask (4Byte)
196   default Gateway (4Byte)
200   NTP Server IP Address (4Byte)
204   ESSID (최대 31자 NULL 종단)
 …
232
236   Device Status (4Byte)
240   Station Type (2Byte) | Priority CH (2Byte) (*1)
244   IEEE802.11r | IEEE802.11ai | IEEE802.11k | IEEE802.11v
248   ┐ Reserve
284   ┘
288   WLAN#1 MAC Address (6Byte) | WLAN#1 Mode | WLAN#1 Band Width
292
296   WLAN#1 FREQ (2Byte) | WLAN#1 Channel Number (2Byte)
300   WLAN#1 Status (2Byte) | WLAN#1 SNR | WLAN#1 RSSI
304   WLAN#1 Connect AP MAC Address (6Byte)        ← 무선기 정보 #1
308
312   ┐ Reserve
348   ┘
352   WLAN#2 MAC Address (6Byte) | WLAN#2 Mode | WLAN#2 Band Width
356
360   WLAN#2 FREQ (2Byte) | WLAN#2 Channel Number (2Byte)
364   WLAN#2 Status (2Byte) | WLAN#2 SNR | WLAN#2 RSSI
368   WLAN#2 Connect AP MAC Address (6Byte) (*1)   ← 무선기 정보 #2
372
376   ┐ Reserve
412   ┘
```
> (*1) Priority CH와 무선 정보 #2 항목은, Station Type이 **Dual Station일 때만** 설정된다.

각 필드 설명은 [3.3.4 필드 상세](#3344-장치-정보-취득-필드-상세) 참조 (원문 16~18페이지).

##### 3.3.4 (필드 상세)

- ◆ **Result/Error Cause** (Login 계열 공통):
  - 정상 시(0x0000), Login 발행 위반(0x0001: 장치 기동 중 발행), Login 조건 위반(0x0002: 2중 발행/타 IP), 패킷 사이즈 지정 오류(0x0003), **Indication 통지 설정 위반(0x0010): Indication 통지 설정이 유효한 상태에서 발행 검출**
- ◆ **Vendor Code**: IEEE 등록 OUI(3byte). 예) CONTEC(0x0000804C), CANTOPS(0x00902CFB)
- ◆ **Product Code**(장치 형식) / **Product Sub Code**(서브 형식, 국가별): FXE3000/FXE5000 등
- ◆ **Date of Manufacture**(제조 연월일): 예 2026/02/28 → 16진수 0x07EA021C (0xYYYYMMDD)
- ◆ **Date of Shipment**(출하 연월일): 동일 표기
- ◆ **Firmware Version**: 펌웨어 버전 및 빌드 번호 포함 문자열 (최대 31자 NULL 종단)
- ◆ **Hardware Version**: 하드웨어 버전 문자열 (최대 31자 NULL 종단)
- ◆ **Serial number OR Device-specific number**: 시리얼 번호 또는 장치별 개체 번호 문자열 (최대 31자 NULL 종단)
- ◆ **Ethernet MAC Address**: 유선 LAN MAC 주소 (6byte)
- ◆ **IP Address**: 장치 IP 주소 (4Byte, 192.168.0.1 = 0xC0A80001)
- ◆ **Subnet Mask**: 서브넷 마스크 (4Byte, 255.255.255.0 = 0xFFFFFF00)
- ◆ **Default Gateway**: 기본 게이트웨이 (4Byte, 192.168.0.2 = 0xC0A80002)
- ◆ **NTP Server IP Address**: NTP 서버 IP 주소 (4Byte, 192.168.0.8 = 0xC0A80008)
- ◆ **ESSID**: Extended Service Set Identifier 지정. 최대 31자(NULL 종단). AP가 스텔스 설정인 경우에는 NULL 종단만 설정된다.
- ◆ **Device Status**: 장치 상태 (기동 중 0x00000000 / 기동 완료 0x00000001 / Login 중 0x00000002)
- ◆ **Station Type**: Single Station=0x0001 / Dual Station=0x0002
- ◆ **Priority CH**: 우선 통신 채널 (Station Type이 Dual Station일 때만 지정. 상위 1Byte=주파수대, 하위 1Byte=CH 번호)
  ```
  Priority CH (2Byte) = [주파수대(Band) | 채널 번호(CH number)]
   주파수대: 2.4GHz(0x01), 5GHz(0x02), 6GHz(0x06)  ← IEEE802.11 정의값
   채널 번호: 주파수대별 CH 번호 (예: 2.4GHz(ISM)대 1~11)
  ```
- ◆ **IEEE802.11r**: Fast BSS Transition 지원. 없음(0x00)/있음(0x01)
- ◆ **IEEE802.11ai**: Fast Initial Link Setup 지원. 없음(0x00)/있음(0x01)
- ◆ **IEEE802.11k**: Radio Resource Measurement 지원. 없음(0x00)/있음 ON(0x01)
- ◆ **IEEE802.11v**: BSS Transition Management 지원. 없음(0x00)/있음 ON(0x01)
- ◆ **WLAN#1/2 MAC Address**: 무선 송수신기 MAC 주소
- ◆ **WLAN#1/2 Mode**: IEEE 규정 무선 규격값 (표 3-2)

###### 표 3-2 WLAN Mode 설정값
| 무선 규격 | PHY 타입명 | 설정값 |
|---|---|---|
| 802.11b | HR-DSSS | 5 |
| 802.11a | OFDM | 4 |
| 802.11g | ERP | 6 |
| 802.11n | HT | 7 |
| 802.11ac | VHT | 9 |
| 802.11ax | HE | 11 |

- ◆ **WLAN#1/2 Band Width**: 주파수 대역폭 (IEEE802.11 규정값, 표 3-3)

###### 표 3-3 주파수 대역폭 설정값
| 대역폭 | 설정값 |
|---|---|
| 20MHz | 0 |
| 40MHz | 1 |
| 80MHz | 2 |
| 160MHz | 3 |
| 80+80MHz | 4 |
| 320MHz | 12 |

- ◆ **WLAN#1/2 FREQ**: 설정 주파수(MHz). 2.4G대 2412MHz→0x096C, 5G대 5180MHz→0x143C, 6G대 6115MHz→0x17E3
- ◆ **WLAN#1/2 Channel Number**: 설정 CH 번호 (주파수대+CH 번호 표기, 위 Priority CH와 동일 형식)
- ◆ **WLAN#1/2 Status**: 무선 LAN 송수신기 접속 상태. 미접속(SCAN 중) 0x0000 / 접속 중 0x0001
- ◆ **WLAN#1/2 SNR**: Signal to Noise Ratio, -128~127. (RSSI에서 노이즈 플로어를 뺀 대노이즈비) `SNR[㏈] = RSSI[dBm] − noise Floor[dBm]`
- ◆ **WLAN#1/2 RSSI**: 수신 신호 강도, -128~127
- ◆ **WLAN#1/2 Connect AP MAC Address**: 접속 중 AP의 MAC 주소(BSSID)

---

#### 3.3.5 패스워드 설정

Login 패스워드의 설정 변경을 수행한다.

- **발행 조건**: Login 처리되어 있을 것
- **불휘발성 메모리 저장**: 있음(有)

**요구 (VHL→무선 기판) 포맷** (Length=312)
```
  0   Protocol Version=1 | Command Type=1 | Request/Indication ID=0x1001
  4   Sequence Number=0xXXXX | Length=312
  8   ┐ Reserve
 60   ┘
 64   old password: 문자열 (최대 127자)  ex "MyPassword_old"
 …
188
192   new password: 문자열 (최대 127자)  ex "MyPassword_new"
 …
316
```
- ◆ **old password**: 구 패스워드 문자열 (최대 127자, NULL 종단)
- ◆ **new password**: 신 패스워드 문자열 (최대 127자, NULL 종단)

**응답 (무선 기판→VHL) 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x1001
  4   Sequence Number=0xXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Result | Error Cause
```
- ◆ **Result**: OK(0x0000) / NG(0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000)
  - Login 발행 위반(0x0001): Login 미발행 상태/장치 기동 중 발행
  - Login 조건 위반(0x0002): Login한 IP와 다른 IP로부터의 요구 수신
  - 패킷 사이즈 지정 오류(0x0003)
  - 불휘발성 메모리 이상(0x0004): 불휘발성 메모리 소거·재기록 이상
  - 구 패스워드 오류(0x00010): 패스워드 불일치
  - 구 패스워드 무효 문자 지정(0x00011): 무효 문자 포함
  - 구 패스워드 NULL 종단 위반(0x00012): 패스워드가 128자인 경우
  - 신 패스워드 무효 문자 지정(0x00013)
  - 신 패스워드 NULL 종단 위반(0x00014): 신 패스워드 문자열이 128자인 경우

---

#### 3.3.6 IP 주소 리스트 설정

무선 장치에 대해, IP 주소·서브넷 마스크·기본 게이트웨이·NTP Server IP 주소 설정과 ESSID 설정값을 하나의 설정 리스트로 하여, 주소 리스트를 변경한다. IP 주소 리스트 번호는 **1~128**까지로, 최대 128개의 IP 주소 리스트 설정이 가능하다.

**IP 주소 리스트에 대하여**

무선 단말 장치의 컨피그레이션 파일로서 보유·설정되며, 아래 항목을 1개의 설정 단위로 하여 128개 리스트를 보유 가능하게 한다. IP 주소 리스트에서 관리되는 항목:

1. 장치 IP 주소
2. 서브넷 마스크
3. 기본 게이트웨이 IP 주소
4. ESSID (31자 이내)
5. NTP Server IP 주소

본 항목을 1개의 IP 주소 리스트로 관리하며, 아래 포맷의 컨피그 파일로 무선 단말에 128개 주소 리스트를 설정 가능하게 한다. 또한 본 IP 주소 리스트 파일을 교체함으로써 IP 주소 리스트의 일괄 변경이 가능하다.

**그림 3-5 IP 주소 리스트 컨피그레이션 파일 구성 (예시)**

> [보완 2026-06-11] 아래 왼쪽 여백의 `#N`은 파일 줄번호가 아니라 원본 도면(image16)에서
> 별도 박스로 그려진 **컨피그 기억 영역의 리스트 번호**다. 파일의 5줄
> (IPAddress/SubnetMask/DefaultGateway/EssId/NTPServer)이 리스트 1개(#N)에 대응한다 —
> 줄마다 #1, #2, #3…이 매겨지는 것이 아님에 주의.

```
#1   IPAddress1=172.17.101.17
#2   IPSubnetMask1=255.255.192.0
#3   IPDefaultGateway1=172.17.65.254
#5   WL54EssId1="XXXX_LV10"
#6   NTPServer1=172.17.2.1
#7   IPAddress2=172.16.101.17
#8   IPSubnetMask2=255.255.192.0
     IPDefaultGateway2=172.16.65.254
#20  WL54EssId2="XXXX_LV30"
     NTPServer2=172.16.2.1
 ⋮
     IPAddress3=172.19.101.17
     IPSubnetMask3=255.255.192.0
#127
#128
```

**불휘발 저장 동작**

본 커맨드는 1회에 최대 **20개**의 IP 주소 리스트 수정이 가능하지만, 복수 회 발행하면 128개 전체 수정이 가능하다.

IP 주소 리스트(128개)는 불휘발 메모리에 저장되며, 장치 기동 시 불휘발에서 값을 읽어 관리된다. 본 커맨드 발행 시에는 재기록용 임시(temporary) 메모리 영역의 값을 교체하고, 최종 IP 주소 리스트 수신과 함께 불휘발 메모리에 저장하여 리스트를 교체한다.

IP 주소 리스트에는 리스트의 개시·종료를 나타내는 플래그(**List Boundary Flag**)가 설정되며, 개시(0x0001), 계속(0x0000), 완료(0x0002), 개시 또한 완료(0x0003)가 설정된다.

> ⚠ 주의: 본문 22페이지의 List Boundary Flag 값 표기 — 개시(0x0001)/계속(0x0000)/완료(0x0002)/개시+완료(0x0003) — 와, 24페이지 필드 설명의 표기 — 개시(0x0000)/계속(0x0001)/종료(0x0002) — 가 **원문에서 서로 상이**하다(원본 불일치). 구현 시 발주처 확인 필요.
>
> [보완 2026-06-11] 원본 docx 재확인 결과, 본 번역본에서 누락됐던 다음 본문 문장을 발견:
> "**시작 리스트(0x0000)와 계속 리스트(0x0001)를 수신하면, 컨피그 기억 영역에** …" —
> 즉 원본 내 3개 언급 중 2개(이 전이 서술 + 24페이지 필드 설명)가 **개시=0x0000**을 지지한다.
> 코드도 개시=0x0000(page-24, 벤더 확인)을 채택했다(`protocol/commands.h`). 22페이지 본문 단락만 상이 — T2 참조.

불휘발 메모리 재기록 처리(소거+기록)는 리스트 종료 수신 시 실행한다. IP 주소 리스트 변경 처리에서 리스트 종료 수신 전에 IP 주소 변경(3.3.7)을 요구받은 경우, 경합으로 간주하여 IP 주소 변경은 NG 처리된다.

**그림 3-6 IP 주소 리스트 불휘발 저장 시퀀스 이미지**
```
요구 1: ID=0x1002, SN=0x0001, Length=1336
  List Boundary Flag=0x0000, #1  Config list Number=1   → #1 IP Address List
   … (개시/계속 수신 시 컨피그 기억 영역에 기록)
  List Boundary Flag=0x0001, #20 Config list Number=20  → #20 IP Address List
요구 2: ID=0x1002, SN=0x0007, Length=568
  List Boundary Flag=0x0001, #121 Config list Number=121 → #121 IP Address List (64Byte)
   …
  List Boundary Flag=0x0002, #128 Config list Number=128 → #128 IP Address List (64Byte)
  (최종 리스트 0x0002 수신 시 불휘발 메모리에 기록)
```

- **발행 조건**: Login 처리되어 있을 것
- **불휘발성 메모리 저장**: 있음(有)

**요구 (VHL→무선 기판) 포맷** (Length 가변: 120~1336 = 56 + 64×n)
```
  0    Protocol Version=1 | Command Type=1 | Request/Indication ID=0x1002
  4    Sequence Number=0xXXXX | Length=120~1336 (56 + 64*#n, #n=리스트 수)
  8    ┐ Reserve
 60    ┘
 64    #1 List Boundary Flag (2Byte) | #1 Configuration list Number (2Byte)=1
 68    #1 IP Address (4Byte)
 72    #1 SubnetMask (4Byte)
 76    #1 default Gateway (4Byte)
 80    #1 NTP Server IP Address (4Byte)
 84    #1 ESSID (최대 31자 NULL 종단, 32Byte: 84~115)
116    #1 Reserve (12Byte: 116~127)              ← 여기까지 64Byte짜리 리스트 1개 (64~127)
128    ┐
       │  #2 … (동일 구조 반복 — 각 엔트리 64Byte stride, #2는 128부터)
 ⋮     │  최대 20개(#n)까지 탑재 가능
       │  #20 List Boundary Flag | #20 Configuration list Number=20
       │  #20 IP Address / SubnetMask / default Gateway / NTP Server IP / ESSID(32Byte)
       ┘
       #20 Reserve (12Byte)
```
> [정정 2026-06-11] 초판 재구성은 #1의 후미 Reserve를 116 한 행으로만 표기하고 120부터 #2가
> 시작되는 듯 그렸으나, 원본 도면(image26)은 **Reserve가 116~127의 12Byte**(#1 엔트리 64Byte의
> 일부)이고 **#2는 오프셋 128에서 시작**한다. (초판 표기대로면 엔트리가 56Byte가 되어
> Length=56+64n과 모순.) 코드(`OPC_IPCFG_ENTRY_LEN=64`)와 도면이 일치한다.
> ※ 라벨 읽기 주의: 도면 왼쪽 숫자는 각 4Byte 행의 **시작 오프셋**이다. Reserve의 라벨
> 116/120/124는 세 행(116~119, 120~123, 124~127)을 가리키므로 12Byte이며, "116~124 = 8Byte"로
> 읽으면 안 된다(124는 끝이 아니라 마지막 행의 시작). 산술 확인: 필드 52B(4+4+4+4+4+32) +
> Reserve 12B = 64B. (password 도면의 라벨 188과 동일한 규약 — V9 참조)
> 또한 원본 도면(image26)의 우측 주석은 "무선 기판은 128개의 리스트를 관리하고, **지정된 번호의
> 리스트를 갱신한다**(更新する)"로, 커밋이 전체 교체가 아니라 **지정 슬롯 갱신(merge)** 임을
> 도면 차원에서도 확인해 준다.

> 무선 기판은 128개 리스트를 관리하며, 지정된 번호의 리스트를 갱신한다. 무선 장치 측은 Length로부터 설정 리스트 개수를 판단한다.

- ◆ **#n List Boundary Flag**: 리스트 개시·종료 플래그 — 개시(0x0000), 계속(0x0001), 종료(0x0002) ※위 불일치 주의 참조
- ◆ **#n Configuration List Number**: 갱신할 리스트 번호 (1~128)
- ◆ **#n IP Address**: 장치 IP 주소 (4Byte, 192.168.0.1 = 0xC0A80001)
- ◆ **#n Subnet Mask**: 서브넷 마스크 (4Byte, 255.255.255.0 = 0xFFFFFF00)
- ◆ **#n default Gateway**: 기본 게이트웨이 (4Byte, 192.168.0.254 = 0xC0A800FE)
- ◆ **#n NTP Server IP Address**: NTP 서버 IP 주소 (4Byte, 192.168.0.2 = 0xC0A80002)
- ◆ **#n ESSID**: ESSID 지정. 최대 31자 (NULL 종단)

**응답 (무선 기판→VHL) 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x1002
  4   Sequence Number=0xXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Result | Error Cause
```
- ◆ **Result**: OK(0x0000) / NG(0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000)
  - Login 발행 위반(0x0001), Login 조건 위반(0x0002), 패킷 사이즈 지정 오류(0x0003), 불휘발성 메모리 이상(0x0004)
  - Configuration 번호 위반(0x00010): 1~128 이외의 리스트 번호 지정
  - IP 주소 설정값 이상(0x0011): 0xFFFFFFFF나 0.0.0.0 등 불가능한 IP 지정
  - Netmask 값 지정 이상(0x0012): 마스크 값이 될 수 없는 지정
  - default Gateway IP 지정 이상(0x0013): IP 주소와 다른 세그먼트의 GW 지정
  - NTP Server IP Address 지정 이상(0x0014): 0xFFFFFFFF/0.0.0.0 등 지정 불가 IP
  - ESSID 지정 문자 이상(0x0015): 문자열로 지정할 수 없는 값
  - ESSID NULL 종단 오류(0x0016): NULL 종단이 없는 경우
  - 리스트 사이즈 지정 오류(0x0017): Length가 56 + 64×n (n=리스트 변경 수)이 아닌 경우

---

#### 3.3.7 IP 주소 변경

무선 기판의 리셋 없이 IP 주소를 변경한다. 저장된 IP 주소 컨피그 기억 영역에서, 지정된 리스트 번호의 IP 주소·NTP Server·ESSID 등으로 변경하도록 지시받는다.

IP 주소 변경은 **Logout 처리 수신으로 응답 송신 후에 실시**된다(Logout 응답 송신까지 대기). 불휘발성 메모리에는 저장하지 않고 설정 변경만 실시한다(전원 재투입 시에는 컨피그레이션에 설정된 IP 주소 내용으로 복귀).

- **발행 조건과 처리 실행 이미지**: Login 설정되어 있을 것이 본 요구 처리의 발행 조건이다. 실행 처리 흐름:
  1. Login의 Request/Response
  2. IP 주소 변경의 Request/Response
  3. Logout의 Request/Response
  4. 무선기 측이 IP 전환 실시

**그림 3-7 IP 주소 변경 처리 이미지**
```
[VHL(OPC 기판)] ──Login 처리──▶ [무선 기판]
[VHL] ──IP 주소 변경 #25 (SN=11)──▶  IP 주소·ESSID 전환 준비(#25 리스트 체크)
[VHL] ──Logout 처리──▶  Logout 응답 송신 후 #25 내용으로 변경 실시
   (컨피그 기억 영역 #1~#128 중 #25 사용)
```

- **불휘발성 메모리 저장**: 없음(無)

**요구 (VHL→무선 기판) 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=1 | Request/Indication ID=0x1003
  4   Sequence Number=0xXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Reserve (2Byte) | Configuration list Number (2Byte)=1
```
> [정정 2026-06-11] 초판 재구성은 마지막 행 오프셋을 60으로 잘못 전사했었다. 원본 도면
> (image31)의 좌측 라벨은 0/4/8/60/**64**이며, Configuration list Number는 프레임 66~67에
> 위치한다(64~65는 Reserve 2Byte). 코드(`opc_change_ip_address_req_unpack`, body[2])와 일치.
- ◆ **Configuration list Number**: 변경 리스트 번호 (Configuration 기억 영역 번호) 1~128

**응답 (무선 기판→VHL) 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x1003
  4   Sequence Number=0xXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Result | Error Cause
```
- ◆ **Result**: OK(0x0000) / NG(0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000)
  - Login 발행 위반(0x0001), Login 조건 위반(0x0002), 패킷 사이즈 지정 오류(0x0003)
  - Configuration List Number 지정값 이상(0x00010): 1~128 이외 지정
  - Configuration List 이상(0x00011): 지정 리스트 번호에 저장된 데이터가 없는 경우(컨피그 파일/IP 주소 리스트 설정으로 미설정)
  - IP 주소 변경 경합(0x00012): IP 주소 리스트 변경 처리 종료 전에 IP 주소 변경을 요구받음

---

#### 3.3.8 무선 설정 변경

무선 설정 요구 처리를 수행한다. 무선계 컨피그레이션 정보의 변경을 장치 리셋 없이 수행한다. 전원 OFF 시 재설정을 고려하여 불휘발 메모리에 저장한다.

- **발행 조건**: Login 처리되어 있을 것
- **불휘발성 메모리 저장**: 있음(有)

**요구 (VHL→무선 기판) 포맷** (Length=76)
```
  0   Protocol Version=1 | Command Type=1 | Request/Indication ID=0x1004
  4   Sequence Number=XXXXX | Length=76
  8   ┐ Reserve
 60   ┘
 64   Station Type (2Byte) | Priority CH (2Byte) (*1)
 68   WLAN#1 FREQ (2Byte) | WLAN#1 CH (2Byte)
 72   WLAN#1 Mode | WLAN#1 Band Width | Reserve
 76   WLAN#2 FREQ (2Byte)(*1) | WLAN#2 CH (2Byte)(*1)
 80   WLAN#2 Mode(*1) | WLAN#2 Band Width(*1) | Reserve
```
> (*1) Station Type이 Dual Station일 때 유효.
> [정정 2026-06-11] 본 번역본 초판은 76행을 "WLAN#2 CH | WLAN#2 FREQ"로 잘못 전사했었다.
> 원본 docx(無線基板共通制御通信仕様書_Rev1.00_KO.docx)의 §3.3.8 요구 포맷 도면(image33.emf)을
> 직접 확인한 결과 WLAN#2도 WLAN#1과 동일하게 FREQ → CH 순서가 맞다 (T13 해소).

- ◆ **Station Type**: Single Station=0x0001(1주파수로 접속하는 단말) / Dual Station=0x0002(2주파수로 접속하는 단말)
- ◆ **Priority CH**: 우선 통신 채널 지정 (Dual Station일 때만. 상위 1Byte=주파수대, 하위 1Byte=CH 번호)
  - 주파수대: 2.4GHz(0x01)/5GHz(0x02)/6GHz(0x06), 채널: 주파수대별 CH (예 2.4GHz 1~11)
- ◆ **WLAN#1/2 FREQ**: 설정 주파수(MHz). 2.4G 2412MHz→0x096C, 5G 5180MHz→0x143C, 6G 6115MHz→0x17E3
- ◆ **WLAN#1/2 Channel Number**: 설정 CH 번호 (상위 1Byte 주파수대, 하위 1Byte CH 번호)
- ◆ **WLAN#1/2 Mode**: 무선 규격값 (표 3-2와 동일: 11b=5, 11a=4, 11g=6, 11n=7, 11ac=9, 11ax=11)
- ◆ **WLAN#1/2 Band Width**: 주파수 대역폭 (표 3-3과 동일: 20MHz=0, 40MHz=1, 80MHz=2, 160MHz=3, 80+80MHz=4, 320MHz=12)

**응답 (무선 기판→VHL) 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x1004
  4   Sequence Number=0xXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Result | Error Cause
```
- ◆ **Result**: OK(0x0000) / NG(0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000)
  - Login 발행 위반(0x0001), Login 조건 위반(0x0002), 패킷 사이즈 지정 오류(0x0003), 불휘발성 메모리 이상(0x0004)
  - Station Type 이상(0x0010): 미지원 스테이션 종별 지정(규정값 이외)
  - 지정 주파수 이상(0x0011): 미지원 주파수 지정
  - 지정 CH 이상(0x0012): 미지원 CH 조건 지정
  - 무선 규격 이상(0x0013): 미지원 무선 규격 지정
  - 무선 주파수 대역 이상(0x0014): 미지원 무선 대역폭 지정

---

#### 3.3.9 Indication 형 메시지 통지 설정

Indication 형 메시지의 송신 설정을 수행한다. 통지 설정은 **설정 데이터로 저장되지 않는** 설정이다. 리셋이나 전원 ON 후에는 통지형 메시지 수신 설정을 매번 다시 해야 한다. 또한 IP 주소 변경 등으로 IP 주소가 바뀐 경우에도 설정이 파기되어 초기 상태(통지 무효 상태)가 된다. 따라서 IP 주소 변경 시에는 통지처 IP 주소·포트 번호를 포함하여 재설정이 필요하다.

Indication 형은 Query 형 메시지(장치 상태 취득) 대신 사용하는 것을 목적으로 하며, Indication 형 통지 설정을 유효로 한 경우 Query 형에 의한 장치 정보 취득은 무효가 된다.

- **발행 조건**: Login 처리되어 있을 것
- **불휘발성 메모리 저장**: 없음(無)

**요구 (VHL→무선 기판) 포맷** (Length=64)
```
  0   Protocol Version=1 | Command Type=1 | Request/Indication ID=0x1005
  4   Sequence Number=XXXXX | Length=64
  8   ┐ Reserve
 60   ┘
 64   Indication UDP Port (2Byte) | Indication Info (1Byte) | Indication Period (1Byte)
 68   Indication IP Address (4Byte)
```
- ◆ **Indication UDP Port**: 통지 시의 통지처 UDP 포트 번호
- ◆ **Indication Info**: 통지 정보 지정 (8bit 비트 지정)
  - `0x01`: 장치 초기 설정 완료 통지
  - `0x02`: 무선 접속 상태 변화 통지 (접속/절단)
  - `0x04`: 로밍 통지
  - `0x08`: AP로부터의 절단 수신 통지
  - `0x10`: 장치 장애 검출 통지 (무선 기판 이상 검출 시)
  - `0x20`: 장치 리셋 통지 (무선 기판이 자율적으로 리셋하기 전에 통지)
  - `0x80`: Keep Alive 통지 (Indication Period가 0초가 아닌 경우만, 주기 통지)
  - 예) 장치 초기 설정 완료(0x01) + 로밍 통지(0x04) + Keep Alive(0x80) 유효 → `0x85` 설정. 통지 없음은 `0x00`.
- ◆ **Indication Period**: 주기 통지 시간 (0~255초). 0초면 사상 변화 검출 시 즉시 통지. 1~255초면 주기마다 사상 변화가 있으면 통지.
- ◆ **Indication IP Address**: 통지처 IP 주소 지정

**응답 (무선 기판→VHL) 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x1005
  4   Sequence Number=XXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Result | Error Cause
```
- ◆ **Result**: OK(0x0000) / NG(0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000)
  - Login 발행 위반(0x0001), Login 조건 위반(0x0002), 패킷 사이즈 지정 오류(0x0003)
  - 통지 정보 지정 오류(0x0010): 할당되지 않은 통지 Bit 지정
  - 미지원 통지 정보 지정(0x0011): 무선기가 지원하지 않는 통지 정보 지정
  - IP 주소 지정 이상(0x0012): 동일 IP 세그먼트 이외의 통지 주소 이상
  - Login 조건 위반(0x0013): Login 상태가 아닌, 다른 IP 주소로부터의 발행 검출
    ([정정 2026-06-11] 초판의 "Login 상태 아님/다른 IP…" 슬래시 표기는 두 독립 조건(OR)처럼
    읽혔으나, 원본은 쉼표 연결 단일 구문이다.
    [잠정 해석 2026-06-11] 본 커맨드에서 **0x0001 = 기판이 로그인 자체가 안 된 상태에서 발행**,
    **0x0013 = 기판은 로그인되어 있으나 로그인된 IP가 아닌 다른 IP로부터 발행** 으로 해석한다.
    이 경우 §3.3.9의 0x0002(Login 조건 위반)와 역할이 겹치므로 최종 확정은 발주처 문의 — A14 참조)

---

#### 3.3.10 리셋 요구

무선 기판에 강제 리셋을 요구한다. 응답 메시지 송신 후에 리셋한다.

- **발행 조건**: Login 처리되어 있을 것

**요구 (VHL→무선 기판) 포맷** (Length=0)
```
  0   Protocol Version=1 | Command Type=1 | Request/Indication ID=0x2001
  4   Sequence Number=XXXXX | Length=0
      (공통 헤더만, 정보 없음)
```

**응답 (무선 기판→VHL) 포맷** (Length=0 ※헤더상 Length=0, Result/Error Cause 포함)
```
  0   Protocol Version=1 | Command Type=2 | Request/Indication ID=0x2001
  4   Sequence Number=XXXXX | Length=0
  8   ┐ Reserve
 60   ┘
 64   Result | Error Cause
```
- ◆ **Result**: OK(0x0000) / NG(0x0001)
- ◆ **Error Cause**:
  - 정상 시(0x0000)
  - Login 발행 위반(0x0001), Login 조건 위반(0x0002), 패킷 사이즈 지정 오류(0x0003)

---

### 3.4 Indication 형 메시지

Indication 형 메시지 수신 설정에서 지정된 정보 통지마다, 비동기로 사상 변화 시에 통지한다. 아래에 Indication 형 메시지의 ID 일람을 나타낸다.

#### 표 3-4 Indication 형 메시지 ID 일람
| 통지 정보 | Indication ID |
|---|---|
| 장치 초기 설정 완료 통지 | 0x0001 |
| 무선 접속 상태 변화 통지 (접속/절단) | 0x0002 |
| 로밍 통지 | 0x0004 |
| AP로부터의 절단 수신 통지 | 0x0008 |
| 장치 장애 검출 통지 (무선 기판 이상 검출 시) | 0x0010 |
| 장치 리셋 통지 (무선 기판 자율 리셋 전 통지) | 0x0020 |
| Keep Alive (지정 주기로 통지 송신) | 0x0080 |

---

#### 3.4.1 장치 초기 설정 완료 통지

전원 ON이나 리셋 시, 무선 기판이 운용 가능 상태가 될 때까지의 상태를 보고한다.

**통지 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0001
  4   Sequence Number=XXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Status
```
- ◆ **Status**: 통지 상태
  - 초기 개시 상태(리셋 및 Power ON 시): 0x00000000
  - 장치 기동 초기 설정 완료(Login 커맨드 요구 접수 가능 상태): 0x00000001
  - 무선 접속 완료(AP 접속되어 운용 상태): 0x00000002
  - Login 통지(Login 커맨드 요구 처리 완료 시): 0x00000003
  - Logout 통지(Logout 커맨드 요구 처리 완료 시): 0x00000004

---

#### 3.4.2 무선 접속 상태 변화 통지

무선 접속 상태 변화가 있었던 경우, 변화가 있었던 주파수 CH와 사상(절단·접속)을 통지한다.

**통지 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0002
  4   Sequence Number=XXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   WLAN Status | Indication CH
```
- ◆ **WLAN Status**: 통지 상태 — 무선 접속 통지(0x0001) / 무선 절단 통지(0x0002)
- ◆ **Indication CH**: 접속 주파수
  ```
  Indication CH (2Byte) = [주파수대(Band) | 채널 번호(CH number)]
   주파수대: 2.4GHz(0x01)/5GHz(0x02)/6GHz(0x06)  ← IEEE802.11 정의값
   채널: 주파수대별 CH 번호 (예 2.4GHz(ISM)대 1~11)
  ```

---

#### 3.4.3 로밍 통지

로밍 임계값을 밑돈 경우, 해당 무선 주파수와 SNR 값을 통지한다.

**통지 포맷** (Length=68)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0004
  4   Sequence Number=XXXXX | Length=68
  8   ┐ Reserve
 60   ┘
 64   Current SNR | Current RSSI
 68   Connect AP MAC Address (6Byte: 68~73)
 72   (Connect AP MAC 계속: 72~73) | CH Number (2Byte: 74~75)
```
> [정정 2026-06-11] 초판 재구성의 "72 CH Number" 단독 표기는 CH Number가 오프셋 72에서
> 시작하는 것으로 오독됐다. 원본 도면(image42)은 행 68의 MAC(6Byte) 셀이 행 72 좌측
> 절반(72~73)까지 세로 병합되고, CH Number는 우측 절반(74~75)의 독립 셀이다 — 필드 겹침 없음.
> 코드(`indications.c`, body[10..11] = 프레임 74~75)와 도면이 일치한다 (T14 해소).
- ◆ **Current SNR**: 로밍 트리거를 검출한 현재 SNR 값, -128~127. (RSSI에서 노이즈 플로어를 뺀 대노이즈비) `SNR[㏈] = RSSI[dBm] − noise Floor[dBm]`
- ◆ **Current RSSI**: 로밍 트리거 검출 시 현재 수신 신호 강도, -128~127
- ◆ **Connect AP MAC Address**: 로밍 전 현 AP의 MAC 주소
- ◆ **CH Number**: 로밍 검출한 CH 번호 (주파수대+채널 번호)
  - 주파수대: 2.4GHz(0x01)/5GHz(0x02)/6GHz(0x06), 채널: 주파수대별 CH (예 2.4GHz 1~11)

---

#### 3.4.4 AP로부터의 절단 수신 통지

AP로부터 절단을 통지받은 경우, 절단 수신 통지를 수행한다.

**통지 포맷** (Length=68)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0008
  4   Sequence Number=XXXXX | Length=68
  8   ┐ Reserve
 60   ┘
 64   Message ID | Result Code
 68   Disconnect AP MAC Address (6Byte: 68~73)
 72   (Disconnect AP MAC 계속: 72~73) | Reserve (74~75)
```
> [정정 2026-06-11] §3.4.3과 동일한 행 평탄화 문제 — 원본 도면(image44)은 MAC(6Byte) 셀이
> 행 72 좌측 절반(72~73)까지 병합되고 우측 절반(74~75)만 Reserve다.
- ◆ **Message ID**: 절단 메시지 종별 — Disassociation(0x000a) / Deauthentication(0x000c)
- ◆ **Result Code**: 절단 메시지로 통지된 IEEE802.11 규정 이유 코드(reason code)
  - 0x0001 (1): Unspecified reason (특정되지 않은 이유)
  - 0x0004 (4): Disassociated due to inactivity (비활동에 의한 절단)
  - 0x0005 (5): Disassociated because AP is unable to handle all currently associated stations (AP가 모든 접속 완료 스테이션을 처리할 수 없어 절단)
- ◆ **Disconnect AP MAC Address**: 절단 통지한 AP의 MAC 주소

---

#### 3.4.5 장치 장애 검출 통지 (무선 기판 이상 검출 시)

무선 기판의 리소스 이상 등을 검출한 경우, 제어 불능이 되기 전에 장애 검출 통지를 수행한다.

**통지 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0010
  4   Sequence Number=XXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Congestion ID | Current val
```
- ◆ **Congestion ID**: 폭주(輻輳, congestion) 포인트 식별자
  - `0x0001` CPU Congestion / CPU Bottleneck: CPU 사용률이 지속적으로 높아 태스크 실행이 지연되고 시스템 전체 응답 속도가 저하된 상태
  - `0x0002` Memory Congestion / Memory Bottleneck: 메모리 부족으로 데이터가 디스크에 빈번히 스왑 아웃/인(페이징)되어 디스크 I/O가 증대하고 성능이 현저히 저하된 상태
  - `0x0003` Disk I/O Congestion / Disk I/O Bottleneck: 디스크(Flash 메모리) 읽기·쓰기(I/O)가 집중되어 요구 처리를 신속히 실행할 수 없는 상태
  - `0x0004` Network I/O Congestion / Network Bottleneck: 네트워크 인터페이스·케이블 대역폭이 통신 데이터량 대비 부족한 상태
- ◆ **Current Val**: 폭주 검출 시의 검출값을 넣어 통지

---

#### 3.4.6 장치 리셋 통지

무선 기판이 자율적으로 리셋하기 전에 통지한다.

**통지 포맷** (Length=60)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0020
  4   Sequence Number=XXXXX | Length=60
  8   ┐ Reserve
 60   ┘
 64   Reset Cause
```
- ◆ **Reset Cause**: 장치별로 통지 가능한 경우, 리셋 요인 ID(고유 ID)를 통지

---

#### 3.4.7 Keep Alive 통지

무선 기판이 주기적으로 생존 확인 메시지를 통지한다. Indication Period가 0으로 설정된 경우에는 통지하지 않는다.

**통지 포맷** (Length=88)
```
  0   Protocol Version=1 | Command Type=3 | Request/Indication ID=0x0080
  4   Sequence Number=XXXXX | Length=88
  8   ┐ Reserve
 60   ┘
 64   Timestamp   ex "2026-02-16T15:47:00Z"
 …
 92
```
- ◆ **Timestamp**: Keep Alive 송신 시각 정보를 문자열로 설정 (최대 31자, 초 단위 포함)

---

*— 문서 끝 (41 / 41) —*
