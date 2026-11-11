#!/usr/bin/env bash
set -Eeuo pipefail

ACTION="${1:-status}"

WAN_IF="wlan0"
LAN_IF="eth0"
WAN_IP="192.168.100.78"
LAN_IP="192.168.123.164"
TARGET_IP="192.168.123.163"
STATE_FILE="/tmp/g1d_chassis_route_toggle.state"

require_root() {
    if [[ "${EUID}" -ne 0 ]]; then
        echo "ERROR: 需要 root 权限。请使用: sudo $0 ${ACTION}" >&2
        exit 1
    fi
}

require_command() {
    local cmd="$1"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        echo "ERROR: 缺少命令: ${cmd}" >&2
        exit 1
    fi
}

print_matching_rules() {
    local label="$1"
    local pattern="$2"
    shift 2
    local output

    output="$("$@" | awk -v pattern="${pattern}" '$0 ~ pattern { print }')"
    if [[ -z "${output}" ]]; then
        echo "${label}: 未发现相关规则"
        return 0
    fi

    echo "${label}:"
    echo "${output}"
}

require_interface_ip() {
    local iface="$1"
    local ip_addr="$2"

    if ! ip link show "${iface}" >/dev/null 2>&1; then
        echo "ERROR: 未找到网卡 ${iface}" >&2
        exit 1
    fi

    if ! ip -4 addr show dev "${iface}" | grep -q "inet ${ip_addr}/"; then
        echo "ERROR: ${iface} 上未找到预期 IP ${ip_addr}" >&2
        echo "当前 ${iface} 地址:" >&2
        ip -br -4 addr show dev "${iface}" >&2
        exit 1
    fi
}

require_network_shape() {
    require_command ip
    require_command iptables
    require_interface_ip "${WAN_IF}" "${WAN_IP}"
    require_interface_ip "${LAN_IF}" "${LAN_IP}"
}

rule_exists() {
    local table="$1"
    shift

    if [[ "${table}" == "filter" ]]; then
        iptables -C "$@" >/dev/null 2>&1
    else
        iptables -t "${table}" -C "$@" >/dev/null 2>&1
    fi
}

add_rule_if_missing() {
    local table="$1"
    shift

    if rule_exists "${table}" "$@"; then
        echo "已存在规则: ${table} $*"
        return 0
    fi

    if [[ "${table}" == "filter" ]]; then
        iptables -A "$@"
    else
        iptables -t "${table}" -A "$@"
    fi
    echo "已添加规则: ${table} $*"
}

delete_rule_if_present() {
    local table="$1"
    shift

    if ! rule_exists "${table}" "$@"; then
        echo "规则不存在，无需删除: ${table} $*"
        return 0
    fi

    if [[ "${table}" == "filter" ]]; then
        iptables -D "$@"
    else
        iptables -t "${table}" -D "$@"
    fi
    echo "已删除规则: ${table} $*"
}

read_previous_ip_forward() {
    local previous_ip_forward
    previous_ip_forward="$(awk -F= '$1 == "previous_ip_forward" { print $2 }' "${STATE_FILE}")"

    if [[ "${previous_ip_forward}" != "0" && "${previous_ip_forward}" != "1" ]]; then
        echo "ERROR: 状态文件 ${STATE_FILE} 内容异常: previous_ip_forward=${previous_ip_forward}" >&2
        exit 1
    fi

    echo "${previous_ip_forward}"
}

start_route() {
    require_root
    require_network_shape

    if [[ ! -f "${STATE_FILE}" ]]; then
        echo "previous_ip_forward=$(sysctl -n net.ipv4.ip_forward)" > "${STATE_FILE}"
    fi

    read_previous_ip_forward >/dev/null
    sysctl -w net.ipv4.ip_forward=1

    add_rule_if_missing filter FORWARD -i "${WAN_IF}" -o "${LAN_IF}" -d "${TARGET_IP}/32" -j ACCEPT
    add_rule_if_missing filter FORWARD -i "${LAN_IF}" -o "${WAN_IF}" -s "${TARGET_IP}/32" -m state --state RELATED,ESTABLISHED -j ACCEPT
    add_rule_if_missing nat POSTROUTING -d "${TARGET_IP}/32" -o "${LAN_IF}" -j MASQUERADE

    echo
    status_route
}

stop_route() {
    require_root
    require_network_shape

    delete_rule_if_present nat POSTROUTING -d "${TARGET_IP}/32" -o "${LAN_IF}" -j MASQUERADE
    delete_rule_if_present filter FORWARD -i "${LAN_IF}" -o "${WAN_IF}" -s "${TARGET_IP}/32" -m state --state RELATED,ESTABLISHED -j ACCEPT
    delete_rule_if_present filter FORWARD -i "${WAN_IF}" -o "${LAN_IF}" -d "${TARGET_IP}/32" -j ACCEPT

    if [[ ! -f "${STATE_FILE}" ]]; then
        echo "ERROR: 找不到状态文件 ${STATE_FILE}，无法判断 start 前 ip_forward 原值。为避免误关全局转发，已停止。" >&2
        echo "如确认要手动关闭，请执行: sudo sysctl -w net.ipv4.ip_forward=0" >&2
        exit 1
    fi

    sysctl -w "net.ipv4.ip_forward=$(read_previous_ip_forward)"
    rm -f "${STATE_FILE}"

    echo
    status_route
}

status_route() {
    require_network_shape

    echo "=== G1D 临时路由状态 ==="
    echo "外侧接口: ${WAN_IF} ${WAN_IP}"
    echo "内侧接口: ${LAN_IF} ${LAN_IP}"
    echo "目标地址: ${TARGET_IP}"
    echo

    echo "ip_forward:"
    sysctl net.ipv4.ip_forward
    echo

    print_matching_rules "FORWARD 相关规则" "${TARGET_IP}" iptables -S FORWARD
    echo

    print_matching_rules "NAT POSTROUTING 相关规则" "${TARGET_IP}.*-o ${LAN_IF} .*MASQUERADE" iptables -t nat -S POSTROUTING
    echo

    echo "目标连通性:"
    ping -c 2 -W 1 "${TARGET_IP}"
}

case "${ACTION}" in
    start)
        start_route
        ;;
    stop)
        stop_route
        ;;
    status)
        status_route
        ;;
    *)
        echo "用法: sudo $0 {start|stop|status}" >&2
        exit 2
        ;;
esac
