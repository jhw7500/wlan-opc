# wlan-opc on-target 테스트 환경 로더 (source 전용)
#
#   source scripts/test-env.sh              # repo 루트의 test-env.json 사용
#   source scripts/test-env.sh /path/x.json # 다른 config
#   TEST_ENV_JSON=/path/x.json source scripts/test-env.sh
#
# 로드되는 변수 : $VHL  $VHLIP  $PW  $TEST_SSH  $TEST_LISTEN_PORT  $VHLCTL_BIN
#                 $TEST_OPC_HOST  $TEST_OPC_PORT  $TEST_OPC_TIMEOUT
# 로드되는 함수 : vhl (8회 재시도)  vhl_login  vhl_listen  vhl_ssh
#
# 예)  $VHL basic-info
#      vhl_login && $VHL device-info
#      $VHL set-indication --bits 0x80 --period 5 --to $VHLIP:$TEST_LISTEN_PORT
#      vhl_listen                         # 다른 터미널에서 수신기
#      vhl_ssh 'systemctl is-active opcd'

# --- config 경로 결정 (sourced 이므로 exit 대신 return) ---
_te_src="${BASH_SOURCE[0]:-$0}"
_te_here="$(cd "$(dirname "$_te_src")" && pwd)"
_te_cfg="${1:-${TEST_ENV_JSON:-${_te_here}/../test-env.json}}"

if [ ! -f "$_te_cfg" ]; then
  echo "test-env: config 없음: $_te_cfg" >&2
  return 1 2>/dev/null || exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "test-env: python3 필요" >&2
  return 1 2>/dev/null || exit 1
fi

# --- JSON → shell 대입문 (shlex.quote 로 안전) ---
_te_assign="$(python3 - "$_te_cfg" <<'PY'
import json, sys, shlex
d = json.load(open(sys.argv[1]))
def g(path, default=""):
    v = d
    for k in path.split("."):
        v = v.get(k) if isinstance(v, dict) else None
        if v is None:
            return default
    return v
def emit(name, path, default=""):
    print(f"{name}={shlex.quote(str(g(path, default)))}")
emit("TEST_OPC_HOST",    "opc.host")
emit("TEST_OPC_PORT",    "opc.port")
emit("TEST_OPC_TIMEOUT", "opc.timeout_ms")
emit("VHLIP",            "vhl.ip")
emit("TEST_LISTEN_PORT", "vhl.listen_port")
emit("PW",               "auth.password")
emit("VHLCTL_BIN",       "paths.vhlctl")
print(f'TEST_SSH={shlex.quote(str(g("ssh.user"))+"@"+str(g("ssh.host")))}')
PY
)" || { echo "test-env: JSON 파싱 실패 ($_te_cfg)" >&2; return 1 2>/dev/null || exit 1; }
eval "$_te_assign"

# --- vhlctl 경로: 상대경로면 config(=repo 루트) 기준 절대경로로 (CWD 무관) ---
_te_root="$(cd "$(dirname "$_te_cfg")" && pwd)"
case "$VHLCTL_BIN" in
  /*) : ;;
  *)  VHLCTL_BIN="${_te_root}/${VHLCTL_BIN#./}" ;;
esac

export VHLCTL_BIN VHLIP PW TEST_SSH TEST_LISTEN_PORT TEST_OPC_HOST TEST_OPC_PORT TEST_OPC_TIMEOUT
export VHL="$VHLCTL_BIN --host $TEST_OPC_HOST --port $TEST_OPC_PORT --timeout $TEST_OPC_TIMEOUT"

# --- 편의 함수 ---
vhl()        { local t; for t in $(seq 1 8); do $VHL "$@" && return 0; echo "  ..retry $t" >&2; done; return 1; }
vhl_login()  { $VHL login --password "$PW"; }
vhl_listen() { "$VHLCTL_BIN" --hex listen --bind "0.0.0.0:${TEST_LISTEN_PORT}"; }
vhl_ssh()    { ssh "$TEST_SSH" "$@"; }

# --- 요약 (비번 마스킹) ---
echo "loaded: OPC=${TEST_OPC_HOST}:${TEST_OPC_PORT} VHL=${VHLIP} listen=${TEST_LISTEN_PORT} pw=*** bin=${VHLCTL_BIN}"
echo "  vars: \$VHL \$VHLIP \$PW \$TEST_SSH \$TEST_LISTEN_PORT \$VHLCTL_BIN"
echo "  fns : vhl(재시도) vhl_login vhl_listen vhl_ssh"
[ -x "$VHLCTL_BIN" ] || echo "  warn: vhlctl 미빌드 → 'make native' 필요 ($VHLCTL_BIN)" >&2

unset _te_src _te_here _te_cfg _te_assign _te_root
