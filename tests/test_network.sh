#!/bin/sh
# Only child network namespace devices are changed.
set -eu
binary=$(readlink -f "${1:-./fenced-run}")
if [ "${2:-}" != --inside ]; then
    exec unshare -Urn "$0" "$binary" --inside
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
ip link add shaped0 type dummy
ip link set shaped0 up
cat > "$tmp/partial.sh" <<'EOF'
#!/bin/sh
tc qdisc replace dev shaped0 root cake bandwidth 2mbit
printf 'applied\n' > "$1"
exec sleep 30
EOF
chmod +x "$tmp/partial.sh"
rc=0
"$binary" run 250 "$tmp/writer.lock" -- "$tmp/partial.sh" "$tmp/effect" || rc=$?
test "$rc" -eq 124
test -s "$tmp/effect"
tc qdisc show dev shaped0 | grep -q 'qdisc cake.*bandwidth 2Mbit'
# Timeout did not roll back the partial write. Reconcile under the same lock.
"$binary" run 1500 "$tmp/writer.lock" -- "$(command -v tc)" qdisc replace dev shaped0 root cake bandwidth 1mbit
tc qdisc show dev shaped0 | grep -q 'qdisc cake.*bandwidth 1Mbit'
echo 'real tc partial-write, timeout and serialized recovery passed'
