#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
#
# This tests basic flowtable functionality.
# Creates following topology:
#
# Originator (MTU 9000) <-Router1-> MTU 1500 <-Router2-> Responder (MTU 2000)
# Router1 is the one doing flow offloading, Router2 has no special
# purpose other than having a link that is smaller than either Originator
# and responder, i.e. TCPMSS announced values are too large and will still
# result in fragmentation and/or PMTU discovery.

# Kselftest framework requirement - SKIP code is 4.
ksft_skip=4
ret=0

ns1in=""
ns2in=""
ns1out=""
ns2out=""

log_netns=$(sysctl -n net.netfilter.nf_log_all_netns)

nft --version > /dev/null 2>&1
if [ $? -ne 0 ];then
	echo "SKIP: Could not run test without nft tool"
	exit $ksft_skip
fi

ip -Version > /dev/null 2>&1
if [ $? -ne 0 ];then
	echo "SKIP: Could not run test without ip tool"
	exit $ksft_skip
fi

which nc > /dev/null 2>&1
if [ $? -ne 0 ];then
	echo "SKIP: Could not run test without nc (netcat)"
	exit $ksft_skip
fi

ip netns add nsr1
if [ $? -ne 0 ];then
	echo "SKIP: Could not create net namespace"
	exit $ksft_skip
fi

ip netns add ns1
ip netns add ns2

ip netns add nsr2

cleanup() {
	for i in 1 2; do
		ip netns del ns$i
		ip netns del nsr$i
	done

	rm -f "$ns1in" "$ns1out"
	rm -f "$ns2in" "$ns2out"

	[ $log_netns -eq 0 ] && sysctl -q net.netfilter.nf_log_all_netns=$log_netns
}

trap cleanup EXIT

sysctl -q net.netfilter.nf_log_all_netns=1

ip link add veth0 netns nsr1 type veth peer name eth0 netns ns1
ip link add veth1 netns nsr1 type veth peer name veth0 netns nsr2

ip link add veth1 netns nsr2 type veth peer name eth0 netns ns2

for dev in lo veth0 veth1; do
  for i in 1 2; do
    ip -net nsr$i link set $dev up
  done
done

ip -net nsr1 addr add 10.0.1.1/24 dev veth0
ip -net nsr1 addr add dead:1::1/64 dev veth0

ip -net nsr2 addr add 10.0.2.1/24 dev veth1
ip -net nsr2 addr add dead:2::1/64 dev veth1

# set different MTUs so we need to push packets coming from ns1 (large MTU)
# to ns2 (smaller MTU) to stack either to perform fragmentation (ip_no_pmtu_disc=1),
# or to do PTMU discovery (send ICMP error back to originator).
# ns2 is going via nsr2 with a smaller mtu, so that TCPMSS announced by both peers
# is NOT the lowest link mtu.

ip -net nsr1 link set veth0 mtu 9000
ip -net ns1 link set eth0 mtu 9000

ip -net nsr2 link set veth1 mtu 2000
ip -net ns2 link set eth0 mtu 2000

# transfer-net between nsr1 and nsr2.
# these addresses are not used for connections.
ip -net nsr1 addr add 192.168.10.1/24 dev veth1
ip -net nsr1 addr add fee1:2::1/64 dev veth1

ip -net nsr2 addr add 192.168.10.2/24 dev veth0
ip -net nsr2 addr add fee1:2::2/64 dev veth0

for i in 1 2; do
  ip netns exec nsr$i sysctl net.ipv4.conf.veth0.forwarding=1 > /dev/null
  ip netns exec nsr$i sysctl net.ipv4.conf.veth1.forwarding=1 > /dev/null

  ip -net ns$i link set lo up
  ip -net ns$i link set eth0 up
  ip -net ns$i addr add 10.0.$i.99/24 dev eth0
  ip -net ns$i route add default via 10.0.$i.1
  ip -net ns$i addr add dead:$i::99/64 dev eth0
  ip -net ns$i route add default via dead:$i::1
  ip netns exec ns$i sysctl net.ipv4.tcp_no_metrics_save=1 > /dev/null

  # don't set ip DF bit for first two tests
  ip netns exec ns$i sysctl net.ipv4.ip_no_pmtu_disc=1 > /dev/null
done

ip -net nsr1 route add default via 192.168.10.2
ip -net nsr2 route add default via 192.168.10.1

ip netns exec nsr1 nft -f - <<EOF
table inet filter {
  flowtable f1 {
     hook ingress priority 0
     devices = { veth0, veth1 }
   }

   chain forward {
      type filter hook forward priority 0; policy drop;

      # flow offloaded? Tag ct with mark 1, so we can detect when it fails.
      meta oif "veth1" tcp dport 12345 flow offload @f1 counter

      # use packet size to trigger 'should be offloaded by now'.
      # otherwise, if 'flow offload' expression never offloads, the
      # test will pass.
      tcp dport 12345 meta length gt 200 ct mark set 1 counter

      # this turns off flow offloading internally, so expect packets again
      tcp flags fin,rst ct mark set 0 accept

      # this allows large packets from responder, we need this as long
      # as PMTUd is off.
      # This rule is deleted for the last test, when we expect PMTUd
      # to kick in and ensure all packets meet mtu requirements.
      meta length gt 1500 accept comment something-to-grep-for

      # next line blocks connection w.o. working offload.
      # we only do this for reverse dir, because we expect packets to
      # enter slow path due to MTU mismatch of veth0 and veth1.
      tcp sport 12345 ct mark 1 counter log prefix "mark failure " drop

      ct state established,related accept

      # for packets that we can't offload yet, i.e. SYN (any ct that is not confirmed)
      meta length lt 200 oif "veth1" tcp dport 12345 counter accept

      meta nfproto ipv4 meta l4proto icmp accept
      meta nfproto ipv6 meta l4proto icmpv6 accept
   }
}
EOF

if [ $? -ne 0 ]; then
	echo "SKIP: Could not load nft ruleset"
	exit $ksft_skip
fi

# test basic connectivity
ip netns exec ns1 ping -c 1 -q 10.0.2.99 > /dev/null
if [ $? -ne 0 ];then
  echo "ERROR: ns1 cannot reach ns2" 1>&2
  bash
  exit 1
fi

ip netns exec ns2 ping -c 1 -q 10.0.1.99 > /dev/null
if [ $? -ne 0 ];then
  echo "ERROR: ns2 cannot reach ns1" 1>&2
  exit 1
fi

if [ $ret -eq 0 ];then
	echo "PASS: netns routing/connectivity: ns1 can reach ns2"
fi

ns1in=$(mktemp)
ns1out=$(mktemp)
ns2in=$(mktemp)
ns2out=$(mktemp)

make_file()
{
	name=$1
	who=$2

	SIZE=$((RANDOM % (1024 * 8)))
	TSIZE=$((SIZE * 1024))

	dd if=/dev/urandom of="$name" bs=1024 count=$SIZE 2> /dev/null

	SIZE=$((RANDOM % 1024))
	SIZE=$((SIZE + 128))
	TSIZE=$((TSIZE + SIZE))
	dd if=/dev/urandom conf=notrunc of="$name" bs=1 count=$SIZE 2> /dev/null
}

check_transfer()
{
	in=$1
	out=$2
	what=$3

	cmp "$in" "$out" > /dev/null 2>&1
	if [ $? -ne 0 ] ;then
		echo "FAIL: file mismatch for $what" 1>&2
		ls -l "$in"
		ls -l "$out"
		return 1
	fi

	return 0
}

test_tcp_forwarding()
{
	local nsa=$1
	local nsb=$2
	local lret=0

	ip netns exec $nsb nc -w 5 -l -p 12345 < "$ns2in" > "$ns2out" &
	lpid=$!

	sleep 1
	ip netns exec $nsa nc -w 4 10.0.2.99 12345 < "$ns1in" > "$ns1out" &
	cpid=$!

	sleep 3

	kill $lpid
	kill $cpid
	wait

	check_transfer "$ns1in" "$ns2out" "ns1 -> ns2"
	if [ $? -ne 0 ];then
		lret=1
	fi

	check_transfer "$ns2in" "$ns1out" "ns1 <- ns2"
	if [ $? -ne 0 ];then
		lret=1
	fi

	return $lret
}

make_file "$ns1in" "ns1"
make_file "$ns2in" "ns2"

# First test:
# No PMTU discovery, nsr1 is expected to fragment packets from ns1 to ns2 as needed.
test_tcp_forwarding ns1 ns2
if [ $? -eq 0 ] ;then
	echo "PASS: flow offloaded for ns1/ns2"
else
	echo "FAIL: flow offload for ns1/ns2:" 1>&2
	ip netns exec nsr1 nft list ruleset
	ret=1
fi

# delete default route, i.e. ns2 won't be able to reach ns1 and
# will depend on ns1 being masqueraded in nsr1.
# expect ns1 has nsr1 address.
ip -net ns2 route del default via 10.0.2.1
ip -net ns2 route del default via dead:2::1
ip -net ns2 route add 192.168.10.1 via 10.0.2.1

# Second test:
# Same, but with NAT enabled.
ip netns exec nsr1 nft -f - <<EOF
table ip nat {
   chain postrouting {
      type nat hook postrouting priority 0; policy accept;
      meta oifname "veth1" masquerade
   }
}
EOF

test_tcp_forwarding ns1 ns2

if [ $? -eq 0 ] ;then
	echo "PASS: flow offloaded for ns1/ns2 with NAT"
else
	echo "FAIL: flow offload for ns1/ns2 with NAT" 1>&2
	ip netns exec nsr1 nft list ruleset
	ret=1
fi

# Third test:
# Same as second test, but with PMTU discovery enabled.
handle=$(ip netns exec nsr1 nft -a list table inet filter | grep something-to-grep-for | cut -d \# -f 2)

ip netns exec nsr1 nft delete rule inet filter forward $handle
if [ $? -ne 0 ] ;then
	echo "FAIL: Could not delete large-packet accept rule"
	exit 1
fi

ip netns exec ns1 sysctl net.ipv4.ip_no_pmtu_disc=0 > /dev/null
ip netns exec ns2 sysctl net.ipv4.ip_no_pmtu_disc=0 > /dev/null

test_tcp_forwarding ns1 ns2
if [ $? -eq 0 ] ;then
	echo "PASS: flow offloaded for ns1/ns2 with NAT and pmtu discovery"
else
	echo "FAIL: flow offload for ns1/ns2 with NAT and pmtu discovery" 1>&2
	ip netns exec nsr1 nft list ruleset
fi

KEY_SHA="0x"$(ps -xaf | sha1sum | cut -d " " -f 1)
KEY_AES="0x"$(ps -xaf | md5sum | cut -d " " -f 1)
SPI1=$RANDOM
SPI2=$RANDOM

if [ $SPI1 -eq $SPI2 ]; then
	SPI2=$((SPI2+1))
fi

do_esp() {
    local ns=$1
    local me=$2
    local remote=$3
    local lnet=$4
    local rnet=$5
    local spi_out=$6
    local spi_in=$7

    ip -net $ns xfrm state add src $remote dst $me proto esp spi $spi_in  enc aes $KEY_AES  auth sha1 $KEY_SHA mode tunnel sel src $rnet dst $lnet
    ip -net $ns xfrm state add src $me  dst $remote proto esp spi $spi_out enc aes $KEY_AES auth sha1 $KEY_SHA mode tunnel sel src $lnet dst $rnet

    # to encrypt packets as they go out (includes forwarded packets that need encapsulation)
    ip -net $ns xfrm policy add src $lnet dst $rnet dir out tmpl src $me dst $remote proto esp mode tunnel priority 1 action allow
    # to fwd decrypted packets after esp processing:
    ip -net $ns xfrm policy add src $rnet dst $lnet dir fwd tmpl src $remote dst $me proto esp mode tunnel priority 1 action allow

}

do_esp nsr1 192.168.10.1 192.168.10.2 10.0.1.0/24 10.0.2.0/24 $SPI1 $SPI2

do_esp nsr2 192.168.10.2 192.168.10.1 10.0.2.0/24 10.0.1.0/24 $SPI2 $SPI1

ip netns exec nsr1 nft delete table ip nat

# restore default routes
ip -net ns2 route del 192.168.10.1 via 10.0.2.1
ip -net ns2 route add default via 10.0.2.1
ip -net ns2 route add default via dead:2::1

test_tcp_forwarding ns1 ns2
if [ $? -eq 0 ] ;then
	echo "PASS: ipsec tunnel mode for ns1/ns2"
else
	echo "FAIL: ipsec tunnel mode for ns1/ns2"
	ip netns exec nsr1 nft list ruleset 1>&2
	ip netns exec nsr1 cat /proc/net/xfrm_stat 1>&2
fi

exit $ret
