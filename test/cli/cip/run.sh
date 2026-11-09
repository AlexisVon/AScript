#!/bin/bash
# cip link tests — require -DBUILD_LINKS=ON
set -u

HERE=$(dirname "$(realpath "$0")")
ROOT=$(realpath "$HERE/../../..")
CLI="$ROOT/bin/axc-cli"
LINK_DIR="$ROOT/bin/links"

[ -x "$CLI" ] || { echo "missing $CLI"; exit 1; }
[ -f "$LINK_DIR/cip.so" ] || { echo "missing $LINK_DIR/cip.so (configure with -DBUILD_LINKS=ON)"; exit 1; }
export LD_LIBRARY_PATH="$ROOT/bin:$ROOT/bin/alxlib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$HERE"

failed=0
check() {
    if grep -qF "$2" <<< "$out"; then
        echo "PASS $1"
    else
        echo "FAIL $1 (missing '$2')"
        failed=1
    fi
}

run_file() {
    if ! out=$("$CLI" -p "$LINK_DIR" "$1" 2>&1); then
        echo "$out"
        echo "FAIL $1 exit"
        exit 1
    fi
}

run_file errors.axc
check handle "handle: cip.get handle: expected a handle"
check connect "connect: cip.open 127.0.0.1:1: connection failed"
check listen "listen: cip.listen 127.0.0.1:1: bind or listen port failed"
check type "type: cip.open addr: expected string"
check dupdecl "dupdecl: cip.define_dint A: already declared as DINT -- remove it first"
check count "count: cip.define_int X: count must be 1 or more"
check dims "dims: cip.define_int Y: a 2-D shape is [rows, columns] -- 1 dimensions is not spelled yet"
check dims3 "dims3: cip.define_int Z: a 2-D shape is [rows, columns] -- 3 dimensions is not spelled yet"
check tag "tag: cip.get Grid[x]: element 'x' in 'Grid[x]' is not an index or a range"
check value "value: cip.set A: an integer point takes an int"
check missing_get "missing get=null reason=[cip.get Nope: status 0x05]"
check missing_set "missing set=false reason=[cip.set Nope: not declared -- a define line for it comes first]"
check copy "copy points=DINT alive=true"

run_file loopback.axc
check local "local=[0, [0, 0, 0], false]"
check set_count "set Count(srv)=true"
check read_count "read Count=7"
check server_sees "server sees=[4, 5, 6]"
check range "read range=[5, 6]"
check label "Label long=a label longer than eight"
check raw "Raw=[65, 66, 67, 68]"
check raw_elem "Raw[1]=66"
check raw_range "Raw after=[65, 120, 121, 68]"
check raw_elem0 "Raw after 0=[90, 120, 121, 68]"
check pad "Pad=[0, 0, 0, 104, 105, 0]"
check pad_cell "Pad[0][1]=7"
check row0 "row 0=[0, 0, 0]"
check row1 "row 1=[0, 0, 9]"
check grid_range "range=[0, 0]"
check whole "whole=[[0, 0, 0], [7, 8, 9]]"
check after "after=[[1, 2, 3], [4, 5, 6]]"
check shape "shape=SINT[2][3]"
check missing "missing=null reason=cip.get Nope: status 0x05"
check wait_holds "wait holds=true"
check wait_row "wait row=true"
check wait_out "wait out=false reason=cip.wait Count: still 7 after 150 ms"
check alive "alive=false"

run_file camera.axc
check piece0 "piece 0: wait=true ack=true source=1 station=7 cell=CELL-0001"
check piece1 "piece 1: wait=true ack=true source=2 station=8 cell=CELL-0002"
check piece2 "piece 2: wait=true source=1 station=9 cell=CELL-0003"
check timeout "timeout=false reason=cip.wait CameraArea[0][1]: still 0 after 100 ms"
check clear "after clear=[0, 0, 0]"
check cam_row "row=[0, 0, 0]"
check cam_whole "whole=000 shape=BYTE[1][101]"

# A real client's captured frames against our server: the script fills the table while the replay
# sends bytes a generic Logix-style client emits (a Multiple Service Packet included).
"$CLI" -p "$LINK_DIR" msp.axc >/tmp/axc_cip_serve.txt 2>&1 &
serve=$!
python3 -I msp_replay.py 127.0.0.1:44838 >/tmp/axc_cip_msp.txt 2>&1
replay_rc=$?
wait "$serve"
serve_rc=$?
if [ "$replay_rc" -ne 0 ] || [ "$serve_rc" -ne 0 ]; then
    cat /tmp/axc_cip_serve.txt /tmp/axc_cip_msp.txt
    echo "FAIL msp exit (replay=$replay_rc serve=$serve_rc)"
    exit 1
fi
out=$(cat /tmp/axc_cip_serve.txt /tmp/axc_cip_msp.txt)
check msp_served "served"
check msp_register "register: 28 in, 28 out, session=1"
check msp_request "request 1: 82 in, 55 out, msp status=0x00, 1 reply at 4"
check msp_sub "sub: service=0xcc status=0x00 type=0x00d1 data=00"

if [ "$failed" -eq 0 ]; then
    echo "cip tests passed"
else
    echo "cip tests FAILED"
fi
exit "$failed"
