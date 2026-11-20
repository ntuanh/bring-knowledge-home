#!/usr/bin/env bash
# Prints the first free TCP port on 127.0.0.1 in a range (default 8100-8999).
#
# Usage: scripts/find_free_port.sh [START] [END]
#
# A port counts as free only if both checks pass:
#   1. `ss -ltn` shows no listening socket on that port, and
#   2. Python can actually bind 127.0.0.1:<port>.
# Commonly used development ports are skipped even if they are free.
set -euo pipefail

START="${1:-8100}"
END="${2:-8999}"
AVOID=" 3000 5000 8000 8080 8888 "

# Ports that already have a listening TCP socket (any address).
LISTENING="$(ss -ltnH 2>/dev/null | awk '{print $4}' | sed 's/.*://' | sort -un | tr '\n' ' ')"

for ((port = START; port <= END; port++)); do
    case "$AVOID" in *" $port "*) continue ;; esac
    case " $LISTENING " in *" $port "*) continue ;; esac
    if python3 - "$port" <<'EOF'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    s.bind(("127.0.0.1", int(sys.argv[1])))
except OSError:
    sys.exit(1)
finally:
    s.close()
EOF
    then
        echo "$port"
        exit 0
    fi
done

echo "find_free_port.sh: no free port in $START-$END" >&2
exit 1
