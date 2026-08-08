#!/usr/bin/env bash
#
# install.sh - Interactive installer for the Acer Nitro fan-control driver (EC or WMI)
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
PKGVER="1.0.0"

EC_NAME="acer-nitro-ec"
WMI_NAME="acer-nitro-wmi"

# ---------- helpers ----------
err()  { echo -e "\e[31m[ERROR]\e[0m $*" >&2; }
info() { echo -e "\e[36m[INFO]\e[0m $*"; }
ok()   { echo -e "\e[32m[OK]\e[0m $*"; }

require_root() {
	if [[ $EUID -ne 0 ]]; then
		err "This script needs root privileges. Run it with: sudo $0"
		exit 1
	fi
}

check_headers() {
	local kdir="/lib/modules/$(uname -r)/build"
	if [[ ! -d "$kdir" ]]; then
		err "Kernel headers not found at $kdir"
		err "Install the linux-headers package matching your kernel first."
		exit 1
	fi
}

is_dkms_installed() {
	# $1 = dkms module name
	dkms status "$1" 2>/dev/null | grep -q "$1"
}

remove_dkms_module() {
	# $1 = module name, $2 = version
	local name="$1" ver="$2"
	if is_dkms_installed "$name"; then
		info "Removing old dkms module: $name/$ver"
		modprobe -r "$name" 2>/dev/null || true
		dkms remove "$name/$ver" --all 2>/dev/null || true
	fi
}

install_variant() {
	# $1 = "ec" | "wmi"
	local variant="$1"
	local src_dir mod_name

	if [[ "$variant" == "ec" ]]; then
		src_dir="$SCRIPT_DIR/ec"
		mod_name="$EC_NAME"
	else
		src_dir="$SCRIPT_DIR/wmi"
		mod_name="$WMI_NAME"
	fi

	if [[ ! -d "$src_dir" ]]; then
		err "Folder $src_dir not found."
		exit 1
	fi

	# The two variants are mutually exclusive -> remove the other one first if present
	if [[ "$variant" == "ec" ]]; then
		remove_dkms_module "$WMI_NAME" "$PKGVER"
	else
		remove_dkms_module "$EC_NAME" "$PKGVER"
	fi

	local dest="/usr/src/${mod_name}-${PKGVER}"

	info "Copying source to $dest ..."
	rm -rf "$dest"
	mkdir -p "$dest"
	cp "$src_dir"/*.c "$src_dir/Makefile" "$src_dir/dkms.conf" "$dest/"
	sed -i "s/@PKGVER@/$PKGVER/" "$dest/dkms.conf"

	info "Adding module to the dkms tree..."
	dkms add -m "$mod_name" -v "$PKGVER"

	info "Building module ($mod_name)..."
	dkms build -m "$mod_name" -v "$PKGVER"

	info "Installing module ($mod_name)..."
	dkms install -m "$mod_name" -v "$PKGVER"

	info "Loading module into the kernel..."
	modprobe "$mod_name"

	ok "Driver $mod_name has been installed and loaded."
	echo
	info "Check with: lsmod | grep $mod_name"
	info "Check logs: dmesg | tail -n 30"
}

uninstall_all() {
	remove_dkms_module "$EC_NAME" "$PKGVER"
	remove_dkms_module "$WMI_NAME" "$PKGVER"
	rm -rf "/usr/src/${EC_NAME}-${PKGVER}" "/usr/src/${WMI_NAME}-${PKGVER}"
	ok "All Acer Nitro driver variants have been removed."
}

show_menu() {
	echo "========================================================"
	echo " Acer Nitro Fan Control Driver Installer"
	echo "========================================================"
	echo " 1) Install EC version   ($EC_NAME)"
	echo " 2) Install WMI version  ($WMI_NAME)"
	echo " 3) Uninstall all variants"
	echo " 4) Exit"
	echo "--------------------------------------------------------"
	read -rp "Choose an option [1-4]: " choice
	case "$choice" in
		1) install_variant "ec" ;;
		2) install_variant "wmi" ;;
		3) uninstall_all ;;
		4) exit 0 ;;
		*) err "Invalid option."; exit 1 ;;
	esac
}

main() {
	require_root
	check_headers

	# Also supports non-interactive mode: ./install.sh ec | wmi | uninstall
	if [[ $# -ge 1 ]]; then
		case "$1" in
			ec)        install_variant "ec" ;;
			wmi)       install_variant "wmi" ;;
			uninstall) uninstall_all ;;
			*) err "Unknown argument: $1 (use: ec | wmi | uninstall)"; exit 1 ;;
		esac
	else
		show_menu
	fi
}

main "$@"
