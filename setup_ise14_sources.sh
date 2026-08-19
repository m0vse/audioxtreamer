#!/usr/bin/env bash

set -eo pipefail

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ise_root=/opt/Xilinx/14.7/ISE_DS
settings="$ise_root/settings64.sh"
pcores="$ise_root/EDK/hw/XilinxProcessorIPLib/pcores"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

if [[ ! -r "$settings" ]]; then
    settings=$(find /opt/Xilinx -type f -name settings64.sh -print -quit 2>/dev/null || true)
fi
[[ -n "$settings" && -r "$settings" ]] || fail "Cannot find the ISE settings64.sh file under /opt/Xilinx."

# Xilinx's settings script defines the ISE command paths.
# shellcheck disable=SC1090
source "$settings"

if [[ ! -d "$pcores" ]]; then
    pcores=$(find /opt/Xilinx -type d -path '*/XilinxProcessorIPLib/pcores' -print -quit 2>/dev/null || true)
fi
[[ -n "$pcores" && -d "$pcores" ]] || fail "Cannot find the XilinxProcessorIPLib pcores directory."

axi_source="$pcores/axi_uartlite_v1_02_a/hdl/vhdl"
proc_source="$pcores/proc_common_v3_00_a/hdl/vhdl"
axi_target="$repo_root/VHDL/axi_uartlite_v1_02_a/hdl/vhdl"
proc_target="$repo_root/VHDL/proc_common_v3_00_a"

[[ -d "$axi_source" ]] || fail "Missing installed AXI UARTLite sources: $axi_source"
[[ -d "$proc_source" ]] || fail "Missing installed proc_common sources: $proc_source"

mkdir -p "$axi_target" "$proc_target"

axi_files=(
    baudrate.vhd
    uartlite_rx.vhd
    uartlite_tx.vhd
)

proc_files=(
    cntr_incr_decr_addn_f.vhd
    dynshreg_f.vhd
    dynshreg_i_f.vhd
    family_support.vhd
    muxf_struct_f.vhd
    proc_common_pkg.vhd
    srl_fifo_f.vhd
    srl_fifo_rbu_f.vhd
)

for file in "${axi_files[@]}"; do
    [[ -f "$axi_source/$file" ]] || fail "Missing installed source: $axi_source/$file"
    cp -f "$axi_source/$file" "$axi_target/$file"
done

for file in "${proc_files[@]}"; do
    [[ -f "$proc_source/$file" ]] || fail "Missing installed source: $proc_source/$file"
    cp -f "$proc_source/$file" "$proc_target/$file"
done

fifo_dir="$repo_root/FPGA/ZTEX201/USB32chAudio/ipcore_dir"
fifo_xco="$fifo_dir/ASYNCH_FIFO_72x256.xco"
fifo_project="$fifo_dir/coregen.cgp"
fifo_vhdl="$fifo_dir/ASYNCH_FIFO_72x256.vhd"

[[ -f "$fifo_xco" ]] || fail "Cannot find FIFO configuration: $fifo_xco"
[[ -f "$fifo_project" ]] || fail "Cannot find Core Generator project: $fifo_project"
command -v coregen >/dev/null 2>&1 || fail "The coregen command is unavailable after loading $settings"

printf 'Generating ASYNCH_FIFO_72x256 from its Core Generator configuration...\n'
(
    cd "$fifo_dir"
    coregen -b "$(basename "$fifo_xco")" -p "$(basename "$fifo_project")"
)

[[ -f "$fifo_vhdl" ]] || fail "Core Generator finished without creating $fifo_vhdl"

printf '\nISE source setup completed successfully.\n'
printf 'Repository: %s\n' "$repo_root"
printf 'EDK library: %s\n' "$pcores"
printf 'You can now reopen FPGA/ZTEX201/USB32chAudio/USB32chAudio.xise.\n'
