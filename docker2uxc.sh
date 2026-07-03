#!/bin/sh
# docker2uxc - convert an OCI/Docker registry image into a uxc OCI runtime bundle
#
# Pulls an image straight from a registry (no docker/podman/skopeo needed),
# flattens its layers into a rootfs (handling AUFS .wh. whiteouts), and emits
# an OCI runtime config.json suitable for OpenWrt's uxc (procd/ujail).
#
# Dependencies: POSIX sh, wget (uclient-fetch or GNU), jq, tar, gzip,
#               sha256sum.  Optional: xz/unxz, zstd (only for those layer types).
#
# SPDX-License-Identifier: MIT

set -eu

VERSION=0.1.0
SELF=${0##*/}
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# profiles: $DOCKER2UXC_PROFILES override, else the in-tree dir (dev), else the
# packaged system path.
if [ -n "${DOCKER2UXC_PROFILES:-}" ]; then
	PROFILE_DIR=$DOCKER2UXC_PROFILES
elif [ -d "$SCRIPT_DIR/profiles" ]; then
	PROFILE_DIR="$SCRIPT_DIR/profiles"
else
	PROFILE_DIR=/usr/share/docker2uxc/profiles
fi

# ------------------------------------------------------------------ defaults --
OUT=""
NAME=""
ARCH=""
CACHE="${DOCKER2UXC_CACHE:-/tmp/docker2uxc-cache}"
VERIFY=1
FORCE=0
VERBOSE=0
RW_OVERLAY=0
NNP=true        # OCI process.noNewPrivileges (secure default); --privileged sets it false
CAPS=permissive
PROFILE=""
NETWORK=host
RESOLVCONF=0
EMIT_NET=0
NET_BRIDGE=br-lan
EMIT_KEEPER=0
ACCOUNTING=1
REGISTER=1
AUTOSTART=0
INFRA=""
UXC_DIR="${DOCKER2UXC_UXCDIR:-/etc/uxc}"
AUTH_FILE="${DOCKER2UXC_AUTH:-/etc/uxcd/auth.json}"   # Docker-format { "auths": { host: { auth | username/password } } }
DOCKERFILE=""
CONTEXT=""
PROV_IMAGE=""   # provenance: the pulled ref + the digest it resolved to, recorded
PROV_DIGEST=""  # in the registry so uxcd can detect updates (direct pulls only)
RESOLVE_ONLY=0  # --resolve-digest: print the digest the ref resolves to and exit
CHECK_UPDATES=0 # --check-updates: report which registered containers have updates

# --------------------------------------------------------------------- usage --
usage() {
	cat <<EOF
$SELF $VERSION - build a uxc OCI bundle from a registry image and register it with uxcd

Usage: $SELF [options] <image-ref>

  <image-ref>   e.g. ghcr.io/blakeblackshear/frigate:0.17.1
                     alpine:3.20            (-> docker.io/library/alpine)
                     quay.io/prometheus/busybox:latest

Options:
  -o, --out DIR        output bundle directory (default: ./<name>)
  -n, --name NAME      container id / hostname (default: repo basename)
  -a, --arch ARCH      amd64 | arm64 | arm/v7 ...  (default: host arch)
      --profile NAME   apply profiles/<NAME>.json overlay (e.g. frigate)
      --network MODE   host | isolated            (default: host)
                         host     = share the host network namespace (no
                                    /etc/config/network setup needed)
                         isolated = own network namespace; you must define an
                                    interface for it in /etc/config/network
      --resolv-conf    bind-mount the host /etc/resolv.conf into the container
      --emit-netconfig write an /etc/config/network veth/infra snippet for the
                       container into the bundle (NOT applied). Implied by
                       --network isolated.
      --net-bridge BR  host bridge to attach the veth to (default: br-lan)
      --emit-keeper    also write <name>.init: a procd "keeper" service giving
                       the container a Docker-style auto-restart policy while
                       keeping it uxc-managed (for apps that restart themselves,
                       e.g. Frigate's "Save & Restart"). Off by default.
      --no-accounting  do NOT add linux.resources; by default a memory+pids
                       resources block (no caps) is added so ujail enables the
                       cgroup controllers and per-container memory/pids stats
                       become available (e.g. for uxcd).
      --caps SET       permissive | minimal       (default: permissive)
      --rw-overlay     tune config for a writable overlay (uxc --write-overlay-path)
      --privileged     set process.noNewPrivileges=false (allow setuid/privilege gain)
      --autostart      register the container to start on boot
      --infra NAME     register the container as a member of shared netns NAME
      --auth-file F    registry credentials, Docker config.json "auths" format
                       (default: \$DOCKER2UXC_AUTH or /etc/uxcd/auth.json)
      --no-register    only build the bundle; do not write $UXC_DIR/<name>.json
      --resolve-digest print the digest the ref resolves to and exit (no build;
                       used by uxcd to detect image updates)
      --check-updates  report which registered containers have a newer image
                       (one line per container: name<TAB>state<TAB>digest)
      --dockerfile F   build from a Dockerfile instead of pulling a ready image
                       (multi-stage FROM..AS + COPY --from=<name|index> supported;
                       RUN/COPY/ADD/ENV/WORKDIR/USER/CMD/ENTRYPOINT; host arch)
      --context DIR    build context for COPY/ADD (default: the Dockerfile's dir)
      --cache DIR      blob cache directory (default: $CACHE)
      --no-verify      skip sha256 digest verification of blobs
  -f, --force          overwrite an existing output directory
  -v, --verbose        verbose progress
  -h, --help           this help

Only anonymous/public images are supported in this version.
EOF
}

# ------------------------------------------------------------------- logging --
log()  { printf '%s\n' "$*" >&2; }
vlog() { [ "$VERBOSE" -eq 1 ] && printf '  %s\n' "$*" >&2 || true; }
die()  { printf '%s: error: %s\n' "$SELF" "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"; }

# --------------------------------------------------------------- arg parsing --
while [ $# -gt 0 ]; do
	case $1 in
		-o|--out)      OUT=$2; shift 2 ;;
		-n|--name)     NAME=$2; shift 2 ;;
		-a|--arch)     ARCH=$2; shift 2 ;;
		--profile)     PROFILE=$2; shift 2 ;;
		--network)     NETWORK=$2; shift 2 ;;
		--resolv-conf) RESOLVCONF=1; shift ;;
		--emit-netconfig) EMIT_NET=1; shift ;;
		--net-bridge)  NET_BRIDGE=$2; shift 2 ;;
		--emit-keeper) EMIT_KEEPER=1; shift ;;
		--no-accounting) ACCOUNTING=0; shift ;;
		--caps)        CAPS=$2; shift 2 ;;
		--rw-overlay)  RW_OVERLAY=1; shift ;;
		--privileged)  NNP=false; shift ;;
		--autostart)   AUTOSTART=1; shift ;;
		--infra)       INFRA=$2; shift 2 ;;
		--no-register) REGISTER=0; shift ;;
		--resolve-digest) RESOLVE_ONLY=1; shift ;;
		--check-updates)  CHECK_UPDATES=1; shift ;;
		--dockerfile)  DOCKERFILE=$2; shift 2 ;;
		--context)     CONTEXT=$2; shift 2 ;;
		--cache)       CACHE=$2; shift 2 ;;
		--auth-file)   AUTH_FILE=$2; shift 2 ;;
		--no-verify)   VERIFY=0; shift ;;
		-f|--force)    FORCE=1; shift ;;
		-v|--verbose)  VERBOSE=1; shift ;;
		-h|--help)     usage; exit 0 ;;
		--version)     printf '%s\n' "$VERSION"; exit 0 ;;
		--)            shift; break ;;
		-*)            die "unknown option: $1 (try --help)" ;;
		*)             break ;;
	esac
done

# --check-updates: for each registered container that has image+digest, resolve
# the current upstream digest (self-exec --resolve-digest) and report
#   <name>\t<current|update|error>\t<digest>
# uxcd runs this as one child for its on-demand update check. No positional ref.
if [ "$CHECK_UPDATES" -eq 1 ]; then
	need jq
	for _f in "$UXC_DIR"/*.json; do
		[ -f "$_f" ] || continue
		_n=$(basename -- "$_f" .json)
		_img=$(jq -r '.image // empty' "$_f" 2>/dev/null)
		_old=$(jq -r '.digest // empty' "$_f" 2>/dev/null)
		[ -n "$_img" ] && [ -n "$_old" ] || continue
		_new=$("$0" --resolve-digest "$_img" 2>/dev/null) || _new=""
		if   [ -z "$_new" ];        then printf '%s\terror\t\n'   "$_n"
		elif [ "$_new" = "$_old" ]; then printf '%s\tcurrent\t%s\n' "$_n" "$_new"
		else                             printf '%s\tupdate\t%s\n'  "$_n" "$_new"; fi
	done
	exit 0
fi

# In Dockerfile mode the base image(s) come from FROM, not a positional argument.
if [ -n "$DOCKERFILE" ]; then
	[ -f "$DOCKERFILE" ] || die "dockerfile not found: $DOCKERFILE"
	[ -n "$CONTEXT" ] || CONTEXT=$(dirname -- "$DOCKERFILE")
else
	[ $# -ge 1 ] || { usage >&2; exit 2; }
	REF=$1
fi

need jq; need tar; need gzip; need sha256sum
command -v wget >/dev/null 2>&1 || need uclient-fetch

case $CAPS in permissive|minimal) ;; *) die "--caps must be permissive or minimal" ;; esac
case $NETWORK in host|isolated) ;; *) die "--network must be host or isolated" ;; esac
[ "$NETWORK" = isolated ] && EMIT_NET=1

# --------------------------------------------------------------- host arch ----
host_arch() {
	case $(uname -m) in
		x86_64|amd64)   echo amd64 ;;
		aarch64|arm64)  echo arm64 ;;
		armv7l)         echo arm/v7 ;;
		armv6l)         echo arm/v6 ;;
		i386|i686)      echo 386 ;;
		*)              uname -m ;;
	esac
}
[ -n "$ARCH" ] || ARCH=$(host_arch)
ARCH_BASE=${ARCH%%/*}
ARCH_VAR=""
[ "$ARCH" != "$ARCH_BASE" ] && ARCH_VAR=${ARCH#*/}

# ===========================================================================
#  functions (definitions only; the build below drives them)
# ===========================================================================

# ----------------------------------------------------------- reference parse --
# Split [registry/]repo[:tag|@digest] from $1 into REG/REPO/TAG/DIGEST/APIHOST
# + REFDESC. Called once per pull (a plain pull, or each image-based stage).
parse_ref() {
	_ref=$1
	REG="" ; REPO="" ; TAG="" ; DIGEST=""
	rest=$_ref
	# digest?
	case $rest in *@*) DIGEST=${rest#*@}; rest=${rest%@*} ;; esac
	# registry = first path component, but only if there's a '/' AND it looks like a
	# host (contains '.' or ':' port, or is localhost). Otherwise it's docker.io.
	case $rest in
		*/*)
			first=${rest%%/*}
			case $first in
				*.*|*:*|localhost) REG=$first; rest=${rest#*/} ;;
				*)                 REG=docker.io ;;
			esac ;;
		*) REG=docker.io ;;
	esac
	# tag = colon in the last path component (never a registry port, which we removed)
	last=${rest##*/}
	case $last in
		*:*)
			TAG=${last##*:}
			newlast=${last%:*}
			case $rest in
				*/*) rest="${rest%/*}/$newlast" ;;
				*)   rest=$newlast ;;
			esac ;;
	esac
	REPO=$rest
	[ -n "$TAG" ] || TAG=latest
	# docker.io: official images live under library/
	if [ "$REG" = "docker.io" ]; then
		case $REPO in */*) ;; *) REPO="library/$REPO" ;; esac
		APIHOST=registry-1.docker.io
	else
		APIHOST=$REG
	fi
	REFDESC=${DIGEST:-$TAG}
}

# ---------------------------------------------------------------- http layer --
# wget wrapper: $1=url $2=outfile, remaining args are extra --header values.
ACCEPT_MANIFEST='application/vnd.oci.image.index.v1+json, application/vnd.docker.distribution.manifest.list.v2+json, application/vnd.oci.image.manifest.v1+json, application/vnd.docker.distribution.manifest.v2+json'
TOKEN=""
BASIC=""   # base64 user:pass for a pure-Basic (no token endpoint) private registry

http_get() {
	_url=$1; _out=$2; shift 2
	set -- "$@"
	if [ -n "$TOKEN" ]; then
		wget -q -O "$_out" --header="Authorization: Bearer $TOKEN" "$@" "$_url"
	elif [ -n "$BASIC" ]; then
		wget -q -O "$_out" --header="Authorization: Basic $BASIC" "$@" "$_url"
	else
		wget -q -O "$_out" "$@" "$_url"
	fi
}

# Resolve base64 "user:pass" (the Docker "auth" value) for $REG from AUTH_FILE,
# or empty. Tries the registry host plus Docker Hub's legacy key aliases, and
# accepts either an "auth" field or separate username/password.
registry_creds() {
	[ -n "$AUTH_FILE" ] && [ -f "$AUTH_FILE" ] || return 0
	case $REG in
		docker.io) _keys='docker.io registry-1.docker.io index.docker.io https://index.docker.io/v1/' ;;
		*)         _keys="$REG" ;;
	esac
	for _k in $_keys; do
		_a=$(jq -r --arg k "$_k" '.auths[$k].auth // empty' "$AUTH_FILE" 2>/dev/null)
		[ -n "$_a" ] && { printf '%s' "$_a"; return 0; }
		_u=$(jq -r --arg k "$_k" '.auths[$k].username // empty' "$AUTH_FILE" 2>/dev/null)
		_p=$(jq -r --arg k "$_k" '.auths[$k].password // empty' "$AUTH_FILE" 2>/dev/null)
		[ -n "$_u" ] && { printf '%s:%s' "$_u" "$_p" | base64 | tr -d '\n'; return 0; }
	done
	return 0
}

get_token() {
	# Per-registry token. With credentials (registry_creds) it is an authenticated
	# token good for private repos; otherwise anonymous. Sets TOKEN, or empty.
	_scope="repository:$REPO:pull"
	_tokurl=""
	case $REG in
		ghcr.io)        _tokurl="https://ghcr.io/token?scope=$_scope" ;;
		docker.io)      _tokurl="https://auth.docker.io/token?service=registry.docker.io&scope=$_scope" ;;
		quay.io)        _tokurl="https://quay.io/v2/auth?service=quay.io&scope=$_scope" ;;
		*)              _tokurl="https://$REG/token?scope=$_scope" ;;
	esac
	_creds=$(registry_creds)
	_tf=$(mktemp)
	if [ -n "$_creds" ]; then
		vlog "using credentials for $REG"
		wget -q -O "$_tf" --header="Authorization: Basic $_creds" "$_tokurl" 2>/dev/null &&
			TOKEN=$(jq -r '.token // .access_token // empty' "$_tf" 2>/dev/null || true)
	else
		wget -q -O "$_tf" "$_tokurl" 2>/dev/null &&
			TOKEN=$(jq -r '.token // .access_token // empty' "$_tf" 2>/dev/null || true)
	fi
	rm -f "$_tf"
	# pure-Basic registry (no token endpoint) but we have credentials: use Basic
	[ -z "$TOKEN" ] && [ -n "$_creds" ] && BASIC=$_creds
	[ -n "$TOKEN" ] && vlog "got token (${#TOKEN} chars)" || vlog "no token (anonymous or Basic registry)"
}

# ------------------------------------------------------------------ blob pull --
blob_path() { echo "$CACHE/${1#sha256:}"; }

fetch_blob() {
	# $1 = digest ; downloads into cache (idempotent), verifies sha256
	_d=$1; _p=$(blob_path "$_d")
	if [ -s "$_p" ]; then
		vlog "cache hit ${_d#sha256:}"
	else
		vlog "fetch ${_d#sha256:}"
		_try=0
		while :; do
			if http_get "https://$APIHOST/v2/$REPO/blobs/$_d" "$_p.part"; then
				break
			fi
			_try=$((_try+1))
			[ "$_try" -ge 3 ] && { rm -f "$_p.part"; die "blob download failed: $_d"; }
			vlog "retry $_try (refreshing token)"
			get_token
		done
		mv "$_p.part" "$_p"
	fi
	if [ "$VERIFY" -eq 1 ]; then
		_sum=$(sha256sum "$_p" | cut -d' ' -f1)
		[ "$_sum" = "${_d#sha256:}" ] || die "digest mismatch for $_d (got $_sum)"
	fi
	echo "$_p"
}

# ------------------------------------------------------------------- flatten ---
decompress_to() {
	# $1 = blob path, $2 = mediaType ; emits a tar stream on stdout
	case $2 in
		*tar+gzip|*tar.gzip|*+gzip) gzip -dc "$1" ;;
		*tar+zstd|*+zstd)
			command -v zstd >/dev/null 2>&1 || die "layer is zstd; install zstd (opkg install zstd)"
			zstd -dc "$1" ;;
		*tar+xz|*+xz)
			if command -v xz >/dev/null 2>&1; then xz -dc "$1"
			else unxz -c "$1"; fi ;;
		*tar) cat "$1" ;;
		*) die "unsupported layer media type: $2" ;;
	esac
}

apply_whiteouts() {
	# $1 = extracted layer dir, $2 = target rootfs
	_ld=$1; _rf=$2
	# opaque dirs first, then file/dir whiteouts
	find "$_ld" -name '.wh..wh..opq' 2>/dev/null | while IFS= read -r m; do
		_rel=${m#"$_ld"/}; _dir=${_rel%/.wh..wh..opq}
		[ "$_dir" = "$_rel" ] && _dir=""   # opq at root
		vlog "opaque $_dir"
		[ -d "$_rf/$_dir" ] && find "$_rf/$_dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
		rm -f "$m"
	done
	find "$_ld" -name '.wh.*' ! -name '.wh..wh..opq' 2>/dev/null | while IFS= read -r m; do
		_rel=${m#"$_ld"/}; _bn=${_rel##*/}; _dir=${_rel%/*}
		[ "$_dir" = "$_rel" ] && _dir=""
		_target=${_bn#.wh.}
		vlog "whiteout ${_dir:+$_dir/}$_target"
		rm -rf "$_rf/${_dir:+$_dir/}$_target"
		rm -f "$m"
	done
}

# Pull an image ref ($1) and flatten every layer into a rootfs dir ($2). Sets
# CONFIG_BLOB (path to the base image config), CONFIG_DIGEST, INDEX_SHA (sha256
# of the manifest the ref resolved to, for provenance) and writes the per-arch
# manifest to $WORK/manifest.json. Shared by a plain pull and each image stage.
pull_base() {
	_pref=$1; _prootfs=$2
	TOKEN=""; BASIC=""                     # fresh auth per registry
	parse_ref "$_pref"
	get_token
	http_get "https://$APIHOST/v2/$REPO/manifests/$REFDESC" "$WORK/idx.json" \
		--header="Accept: $ACCEPT_MANIFEST" \
		|| die "failed to fetch manifest for $_pref (private image, or wrong ref?)"
	INDEX_SHA=$(sha256sum "$WORK/idx.json" | awk '{print $1}')

	_mtype=$(jq -r '.mediaType // ""' "$WORK/idx.json")
	case $_mtype in
		*image.index*|*manifest.list*)
			vlog "multi-arch index; selecting linux/$ARCH"
			_sel=$(jq -r --arg a "$ARCH_BASE" --arg v "$ARCH_VAR" '
				.manifests[]
				| select(.platform.os=="linux"
				         and .platform.architecture==$a
				         and ((.platform.variant // "")==$v))
				| .digest' "$WORK/idx.json" | head -n1)
			[ -n "$_sel" ] || {
				log "available platforms:"
				jq -r '.manifests[] | "    \(.platform.os)/\(.platform.architecture)\(.platform.variant // "")"' "$WORK/idx.json" >&2
				die "no manifest for linux/$ARCH in $_pref"
			}
			http_get "https://$APIHOST/v2/$REPO/manifests/$_sel" "$WORK/manifest.json" \
				--header="Accept: $ACCEPT_MANIFEST" \
				|| die "failed to fetch per-arch manifest for $_pref"
			;;
		*)
			cp "$WORK/idx.json" "$WORK/manifest.json"
			;;
	esac

	CONFIG_DIGEST=$(jq -r '.config.digest' "$WORK/manifest.json")
	[ "$CONFIG_DIGEST" != null ] || die "manifest has no config (unexpected media type: $_mtype)"
	_nlayers=$(jq -r '.layers | length' "$WORK/manifest.json")
	vlog "$_nlayers layer(s), config $CONFIG_DIGEST"

	mkdir -p "$CACHE" "$_prootfs"
	CONFIG_BLOB=$(fetch_blob "$CONFIG_DIGEST")

	# download + flatten each layer straight into $_prootfs
	_i=0
	while [ "$_i" -lt "$_nlayers" ]; do
		_d=$(jq -r ".layers[$_i].digest" "$WORK/manifest.json")
		_mt=$(jq -r ".layers[$_i].mediaType" "$WORK/manifest.json")
		_sz=$(jq -r ".layers[$_i].size" "$WORK/manifest.json")
		vlog "layer $((_i+1))/$_nlayers ${_d#sha256:} ($((_sz/1024/1024)) MB)"
		_bp=$(fetch_blob "$_d")
		if decompress_to "$_bp" "$_mt" | tar -t 2>/dev/null | grep -q '\(^\|/\)\.wh\.'; then
			# safe path: extract to temp, apply whiteouts, merge
			_ldir="$WORK/layer.$$.$_i"
			rm -rf "$_ldir"; mkdir -p "$_ldir"
			decompress_to "$_bp" "$_mt" | tar -x -p -C "$_ldir" 2>/dev/null || true
			apply_whiteouts "$_ldir" "$_prootfs"
			cp -a "$_ldir/." "$_prootfs/" 2>/dev/null || true
			rm -rf "$_ldir"
		else
			# fast path: no whiteouts, extract straight in
			decompress_to "$_bp" "$_mt" | tar -x -p -C "$_prootfs" 2>/dev/null || true
		fi
		_i=$((_i+1))
	done
}

# ------------------------------------------------------- dockerfile build ----
# RUN runs inside a stage's rootfs (chroot, with /proc /dev /sys bound and the
# host resolver for network); COPY/ADD pull files from the build context or, with
# --from=<name|index>, from an earlier stage's rootfs; ENV/WORKDIR/USER/CMD/
# ENTRYPOINT update that stage's image config (read by the config.json step).
mnt_up() {
	mkdir -p "$ROOTFS/proc" "$ROOTFS/dev" "$ROOTFS/sys" "$ROOTFS/etc"
	# clear any stale binds left by a previously crashed build (defensive)
	umount "$ROOTFS/proc" 2>/dev/null || true
	umount "$ROOTFS/dev"  2>/dev/null || true
	umount "$ROOTFS/sys"  2>/dev/null || true
	mount -o bind /proc "$ROOTFS/proc" 2>/dev/null || die "cannot bind /proc into rootfs (need root)"
	mount -o bind /dev  "$ROOTFS/dev"  2>/dev/null || true
	mount -o bind /sys  "$ROOTFS/sys"  2>/dev/null || true
	[ -f /etc/resolv.conf ] && cp -L /etc/resolv.conf "$ROOTFS/etc/resolv.conf" 2>/dev/null || true
}
mnt_down() { for _m in proc dev sys; do umount "$ROOTFS/$_m" 2>/dev/null || true; done; }

cfg_jq() { jq "$@" "$CONFIG_BLOB" > "$WORK/.cfg" && mv "$WORK/.cfg" "$CONFIG_BLOB"; }

# Resolve a COPY --from=<name|index> token ($1) to a built stage's rootfs dir on
# stdout, or return 1 if no such stage was built. Reads $WORK/built.
resolve_from() {
	_t=$1
	case $_t in
		''|*[!0-9]*)   # a stage name (case-insensitive)
			_u=$(printf '%s' "$_t" | tr '[:lower:]' '[:upper:]')
			while IFS="$SEP" read -r _bi _bn _br; do
				[ -n "$_bn" ] || continue
				_bu=$(printf '%s' "$_bn" | tr '[:lower:]' '[:upper:]')
				[ "$_bu" = "$_u" ] && { printf '%s' "$_br"; return 0; }
			done < "$WORK/built"
			return 1 ;;
		*)             # a numeric stage index
			_r=$(awk -F"$SEP" -v n="$_t" '$1==n{print $3}' "$WORK/built")
			[ -n "$_r" ] && { printf '%s' "$_r"; return 0; }
			return 1 ;;
	esac
}

# Apply one stage's instructions. $1 = lines file, $2 = rootfs dir, $3 = the
# stage's (mutable) image config blob. Binds are always torn down before return.
apply_stage() {
	_lines=$1; ROOTFS=$2; CONFIG_BLOB=$3
	command -v chroot >/dev/null 2>&1 || die "chroot not found (needed for Dockerfile RUN)"
	DF_WD=$(jq -r '.config.WorkingDir // "/"' "$CONFIG_BLOB"); [ -n "$DF_WD" ] || DF_WD=/
	DF_ENV=""   # "export K=V; " prefix applied to each RUN
	mnt_up
	while IFS= read -r dl; do
		[ -n "$dl" ] || continue
		kw=$(printf '%s' "$dl" | awk '{print toupper($1)}')
		arg=$(printf '%s' "$dl" | sed 's/^[^[:space:]]*[[:space:]]*//')
		case $kw in
			FROM) ;;   # stage delimiter, not part of a stage body
			RUN)
				log "  RUN $arg"
				chroot "$ROOTFS" /bin/sh -c "${DF_ENV}cd \"$DF_WD\" 2>/dev/null || true
$arg" || { mnt_down; die "RUN failed: $arg"; }
				;;
			COPY|ADD)
				set -- $arg
				_from=""
				while [ $# -gt 1 ]; do
					case $1 in
						--from=*) _from=${1#--from=}; shift ;;
						--*)      shift ;;
						*)        break ;;
					esac
				done
				[ $# -ge 2 ] || { mnt_down; die "$kw needs <src>... <dst>: $dl"; }
				eval "dst=\${$#}"
				ddst="$ROOTFS/$dst"
				case $dst in */) mkdir -p "$ddst" ;; *) mkdir -p "$(dirname "$ddst")" ;; esac
				_srcroot="$CONTEXT"
				if [ -n "$_from" ]; then
					_srcroot=$(resolve_from "$_from") || { mnt_down; die "$kw --from=$_from: no such stage (external-image --from is not supported)"; }
				fi
				_n=1
				for s in "$@"; do
					if [ "$_n" -lt "$#" ]; then
						cp -a "$_srcroot/$s" "$ddst" || { mnt_down; die "$kw: cannot copy $s"; }
					fi
					_n=$((_n+1))
				done
				log "  $kw${_from:+ --from=$_from} -> $dst"
				;;
			ENV)
				case $arg in
					*=*) k=${arg%%=*}; v=${arg#*=} ;;   # K=V (rest of line = value)
					*)   k=${arg%% *}; v=${arg#* } ;;    # legacy: K V
				esac
				case $v in \"*\") v=${v#\"}; v=${v%\"} ;; \'*\') v=${v#\'}; v=${v%\'} ;; esac
				cfg_jq --arg k "$k" --arg v "$v" \
					'.config.Env = ((.config.Env // []) | map(select(startswith($k+"=")|not))) + [$k+"="+$v]'
				DF_ENV="${DF_ENV}export $k=\"$v\"; "
				log "  ENV $k=$v"
				;;
			WORKDIR)
				DF_WD=$arg; mkdir -p "$ROOTFS/$arg"
				cfg_jq --arg d "$arg" '.config.WorkingDir = $d'
				;;
			USER) cfg_jq --arg u "$arg" '.config.User = $u' ;;
			CMD)
				case $arg in \[*) cfg_jq --argjson a "$arg" '.config.Cmd = $a' ;;
				             *)   cfg_jq --arg c "$arg" '.config.Cmd = ["/bin/sh","-c",$c]' ;; esac ;;
			ENTRYPOINT)
				case $arg in \[*) cfg_jq --argjson a "$arg" '.config.Entrypoint = $a' ;;
				             *)   cfg_jq --arg c "$arg" '.config.Entrypoint = ["/bin/sh","-c",$c]' ;; esac ;;
			EXPOSE|VOLUME|LABEL|ARG|MAINTAINER|SHELL|STOPSIGNAL|HEALTHCHECK|ONBUILD)
				vlog "($kw ignored)" ;;
			*) log "  WARNING: unknown Dockerfile instruction ignored: $kw" ;;
		esac
	done < "$_lines"
	mnt_down
}

# ===========================================================================
#  build
# ===========================================================================

# default name: the build-context dir in Dockerfile mode (the base image name
# would be wrong), else the image repo basename.
if [ -z "$DOCKERFILE" ]; then parse_ref "$REF"; SOURCE="$REG/$REPO:$TAG"; fi
if [ -z "$NAME" ]; then
	if [ -n "$DOCKERFILE" ]; then NAME=$(basename -- "$(cd "$CONTEXT" && pwd)"); else NAME=${REPO##*/}; fi
fi
[ -n "$OUT" ]  || OUT="./$NAME"

log "==> arch:     $ARCH"
log "==> bundle:   $OUT  (name: $NAME)"
[ -z "$DOCKERFILE" ] && log "==> image:    $REG/$REPO ${DIGEST:+@$DIGEST}${DIGEST:+ }${DIGEST:-:$TAG}"

WORK=$(mktemp -d)
# Unmount any Dockerfile-build binds BEFORE removing $WORK - otherwise rm -rf
# would recurse into bind-mounted /proc, /dev, /sys (i.e. the host's!). With
# multi-stage there can be several stage rootfs dirs, so umount them all.
cleanup() {
	for _r in "$WORK/rootfs" "$WORK"/stage.*/rootfs; do
		[ -d "$_r" ] || continue
		for _m in proc dev sys; do umount "$_r/$_m" 2>/dev/null || true; done
	done
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# --resolve-digest: print the digest the ref resolves to (manifest-only, no
# blobs) and stop. Used by uxcd's update check; logs go to stderr so stdout is
# just the digest. Pull mode only (a build has no single "image" digest).
if [ "$RESOLVE_ONLY" -eq 1 ]; then
	[ -n "$DOCKERFILE" ] && die "--resolve-digest is for image pulls, not Dockerfile builds"
	get_token
	http_get "https://$APIHOST/v2/$REPO/manifests/$REFDESC" "$WORK/idx.json" \
		--header="Accept: $ACCEPT_MANIFEST" || die "failed to fetch manifest"
	printf 'sha256:%s\n' "$(sha256sum "$WORK/idx.json" | awk '{print $1}')"
	exit 0
fi

# field separator for the stage tables: US (0x1f), a non-whitespace byte so that
# `read` keeps empty fields (an unnamed stage's name) instead of collapsing tabs.
SEP=$(printf '\037')

if [ -n "$DOCKERFILE" ]; then
	# ---- multi-stage Dockerfile build ----
	log "==> dockerfile: $DOCKERFILE (context: $CONTEXT)"

	# logical lines: drop full-line comments, join backslash continuations
	grep -vE '^[[:space:]]*#' "$DOCKERFILE" | awk '
		function emit(s){ sub(/^[ \t]+/,"",s); sub(/[ \t]+$/,"",s); if(s!="") print s }
		{ sub(/\r$/,""); l=$0; if(buf!=""){ l=buf " " l; buf="" }
		  if(l ~ /\\[ \t]*$/){ sub(/\\[ \t]*$/,"",l); buf=l; next } emit(l) }
		END{ if(buf!="") emit(buf) }' > "$WORK/df.lines"

	# segment logical lines into stages: writes $WORK/stages (idx<TAB>name<TAB>ref
	# <TAB>baseidx), per-stage instruction files $WORK/stage.<idx>.lines, the stage
	# count, and $WORK/lastuse (idx<TAB>last-consumer-idx) for eager rootfs freeing.
	awk -v work="$WORK" -v sep="$SEP" '
		BEGIN { OFS = sep; ns = 0 }
		{
			n = split($0, w)
			kw = toupper(w[1])
			if (kw == "FROM") {
				ref=""; name=""
				for (i=2; i<=n; i++) {
					if (w[i] ~ /^--/) continue
					if (ref=="") { ref=w[i]; continue }
					if (toupper(w[i])=="AS" && (i+1)<=n) { name=w[i+1]; break }
				}
				if (ref=="") { print "FROM needs an image or stage name: " $0 > (work "/parse.err"); exit 2 }
				base=-1
				for (k=0; k<ns; k++) if (sname[k]!="" && toupper(sname[k])==toupper(ref)) { base=k; break }
				if (base>=0) lastuse[base]=ns
				sname[ns]=name
				print ns, name, ref, base >> (work "/stages")
				curfile = work "/stage." ns ".lines"
				ns++
				next
			}
			if (ns==0) { print "Dockerfile instruction before the first FROM: " $0 > (work "/parse.err"); exit 2 }
			if (kw=="COPY" || kw=="ADD") {
				for (i=2; i<=n; i++) {
					if (w[i] ~ /^--from=/) {
						t = substr(w[i], 8); ki = -1
						if (t ~ /^[0-9]+$/) ki = t+0
						else { for (k=0;k<ns;k++) if (sname[k]!="" && toupper(sname[k])==toupper(t)) { ki=k; break } }
						if (ki>=0) lastuse[ki] = ns-1
					} else if (w[i] !~ /^--/) break
				}
			}
			print $0 >> curfile
		}
		END {
			print ns+0 > (work "/nstages")
			for (j=0; j<ns; j++) print j, (j in lastuse ? lastuse[j] : -1) >> (work "/lastuse")
		}' "$WORK/df.lines" || true
	[ -f "$WORK/parse.err" ] && die "$(cat "$WORK/parse.err")"
	NSTAGES=$(cat "$WORK/nstages" 2>/dev/null || echo 0)
	[ "$NSTAGES" -ge 1 ] || die "no FROM instruction in $DOCKERFILE"
	log "==> $NSTAGES stage(s)"

	STAGE_LAST=$((NSTAGES - 1))
	: > "$WORK/built"

	while IFS="$SEP" read -r i sname sref sbase; do
		[ -n "$i" ] || continue
		sdir="$WORK/stage.$i"; mkdir -p "$sdir"; scfg="$sdir/config"
		if [ "$i" -eq "$STAGE_LAST" ]; then srfs="$WORK/rootfs"; else srfs="$sdir/rootfs"; fi
		mkdir -p "$srfs"
		log "==> stage $((i+1))/$NSTAGES${sname:+ ($sname)}: FROM $sref"

		if [ "$sbase" -ge 0 ]; then
			# base is an earlier stage: clone its rootfs + inherit its (built) config
			bdir=$(awk -F"$SEP" -v n="$sbase" '$1==n{print $3}' "$WORK/built")
			[ -n "$bdir" ] || die "stage $((i+1)): base stage rootfs missing"
			cp -a "$bdir/." "$srfs/" || die "stage $((i+1)): cannot clone base stage rootfs"
			cp "$WORK/stage.$sbase/config"        "$scfg"             || die "stage $((i+1)): cannot inherit base config"
			cp "$WORK/stage.$sbase/manifest.json" "$sdir/manifest.json" 2>/dev/null || true
			cp "$WORK/stage.$sbase/source"        "$sdir/source"      2>/dev/null || true
			cp "$WORK/stage.$sbase/cfgdigest"     "$sdir/cfgdigest"   2>/dev/null || true
		else
			pull_base "$sref" "$srfs"
			cp "$CONFIG_BLOB" "$scfg"                      # per-stage mutable image config
			cp "$WORK/manifest.json" "$sdir/manifest.json"
			printf '%s\n' "$REG/$REPO:$TAG" > "$sdir/source"
			printf '%s\n' "$CONFIG_DIGEST"  > "$sdir/cfgdigest"
		fi

		[ -f "$WORK/stage.$i.lines" ] && apply_stage "$WORK/stage.$i.lines" "$srfs" "$scfg"
		printf '%s\037%s\037%s\n' "$i" "$sname" "$srfs" >> "$WORK/built"

		# eager free: drop earlier intermediate stage rootfs whose last consumer was this stage
		while IFS="$SEP" read -r lj lu; do
			[ -n "$lj" ] || continue
			{ [ "$lj" -lt "$i" ] && [ "$lj" -ne "$STAGE_LAST" ] && [ "$lu" -le "$i" ]; } || continue
			_fr="$WORK/stage.$lj/rootfs"
			[ -d "$_fr" ] || continue
			for _m in proc dev sys; do umount "$_fr/$_m" 2>/dev/null || true; done
			rm -rf "$_fr"
		done < "$WORK/lastuse"
	done < "$WORK/stages"

	ROOTFS="$WORK/rootfs"
	CONFIG_BLOB="$WORK/stage.$STAGE_LAST/config"
	SOURCE=$(cat "$WORK/stage.$STAGE_LAST/source" 2>/dev/null || echo "")
	CONFIG_DIGEST=$(cat "$WORK/stage.$STAGE_LAST/cfgdigest" 2>/dev/null || echo "")
	cp "$WORK/stage.$STAGE_LAST/manifest.json" "$WORK/manifest.json" 2>/dev/null || true
	log "==> dockerfile build complete ($NSTAGES stage(s))"
else
	# ---- plain image pull ----
	ROOTFS="$WORK/rootfs"; mkdir -p "$ROOTFS"
	log "==> resolving + pulling $REF"
	pull_base "$REF" "$ROOTFS"
	# provenance: the digest this ref resolved to lets uxcd re-resolve + detect updates
	PROV_IMAGE=$REF
	PROV_DIGEST="sha256:$INDEX_SHA"
	log "==> flattened into rootfs"
fi

# --------------------------------------------------- image config -> runtime ---
log "==> generating uxc config.json"

# Resolve User -> uid:gid (numeric, or name looked up in the rootfs).
USER_STR=$(jq -r '.config.User // "" | tostring' "$CONFIG_BLOB")
UID_N=0; GID_N=0
if [ -n "$USER_STR" ]; then
	u=${USER_STR%%:*}; g=""
	case $USER_STR in *:*) g=${USER_STR#*:} ;; esac
	case $u in
		''|*[!0-9]*)
			if [ -f "$ROOTFS/etc/passwd" ]; then
				line=$(grep -E "^$u:" "$ROOTFS/etc/passwd" 2>/dev/null | head -n1 || true)
				[ -n "$line" ] && { UID_N=$(echo "$line" | cut -d: -f3); [ -z "$g" ] && GID_N=$(echo "$line" | cut -d: -f4); }
			fi ;;
		*) UID_N=$u ;;
	esac
	case $g in
		'') : ;;
		*[!0-9]*)
			if [ -f "$ROOTFS/etc/group" ]; then
				line=$(grep -E "^$g:" "$ROOTFS/etc/group" 2>/dev/null | head -n1 || true)
				[ -n "$line" ] && GID_N=$(echo "$line" | cut -d: -f3)
			fi ;;
		*) GID_N=$g ;;
	esac
fi
[ -n "$UID_N" ] || UID_N=0
[ -n "$GID_N" ] || GID_N=0

# Capability sets.
CAPS_PERMISSIVE='["CAP_CHOWN","CAP_DAC_OVERRIDE","CAP_DAC_READ_SEARCH","CAP_FOWNER","CAP_FSETID","CAP_KILL","CAP_SETGID","CAP_SETUID","CAP_SETPCAP","CAP_NET_BIND_SERVICE","CAP_NET_RAW","CAP_NET_ADMIN","CAP_SYS_CHROOT","CAP_MKNOD","CAP_AUDIT_WRITE","CAP_SETFCAP","CAP_IPC_LOCK","CAP_SYS_PTRACE","CAP_SYS_NICE","CAP_SYS_RESOURCE"]'
CAPS_MINIMAL='["CAP_CHOWN","CAP_DAC_OVERRIDE","CAP_FOWNER","CAP_SETGID","CAP_SETUID","CAP_NET_BIND_SERVICE","CAP_KILL"]'
case $CAPS in permissive) CAPSET=$CAPS_PERMISSIVE ;; minimal) CAPSET=$CAPS_MINIMAL ;; esac

RO_ROOT=false
[ "$RW_OVERLAY" -eq 1 ] && RO_ROOT=true   # overlay supplies the writable upper layer

# Network namespace: isolated => own netns (needs a host /etc/config/network
# interface); host => share the host network (no provisioning required).
if [ "$NETWORK" = isolated ]; then NS_NET='{"type":"network"},'; else NS_NET=''; fi
NS_JSON="[{\"type\":\"pid\"},${NS_NET}{\"type\":\"ipc\"},{\"type\":\"uts\"},{\"type\":\"cgroup\"},{\"type\":\"mount\"}]"

# resolv.conf is opt-in: uxc/procd manages container DNS itself, and a stray
# bind can clash with that. Add it only when the user asks.
if [ "$RESOLVCONF" -eq 1 ]; then
	RESOLV_MOUNT='[{"destination":"/etc/resolv.conf","type":"bind","source":"/etc/resolv.conf","options":["rbind","ro"]}]'
else
	RESOLV_MOUNT='[]'
fi

# cgroup accounting: a memory+pids resources block (no real caps) makes ujail
# enable those controllers, so per-container memory/pids stats become readable
# (e.g. by uxcd). memory limit -1 = unlimited; pids needs a concrete number, so
# use the kernel's pid_max (effectively unlimited). Disable with --no-accounting.
if [ "$ACCOUNTING" -eq 1 ]; then
	_pidmax=$(cat /proc/sys/kernel/pid_max 2>/dev/null)
	[ -n "$_pidmax" ] || _pidmax=32768
	RES_JSON="{\"resources\":{\"memory\":{\"limit\":-1},\"pids\":{\"limit\":$_pidmax}}}"
else
	RES_JSON='{}'
fi

jq -n \
	--slurpfile img "$CONFIG_BLOB" \
	--arg name "$NAME" \
	--argjson uid "$UID_N" \
	--argjson gid "$GID_N" \
	--argjson caps "$CAPSET" \
	--argjson roroot "$RO_ROOT" \
	--argjson nnp "$NNP" \
	--argjson ns "$NS_JSON" \
	--argjson resolv "$RESOLV_MOUNT" \
	--argjson res "$RES_JSON" \
	'
	($img[0].config // {}) as $c
	| (($c.Entrypoint // []) + ($c.Cmd // [])) as $args
	| (if ($args|length)==0 then ["/bin/sh"] else $args end) as $args
	| ($c.Env // []) as $env
	| (if any($env[]; startswith("PATH=")) then $env
	   else $env + ["PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"] end) as $env
	| {
	  ociVersion: "1.0.2",
	  hostname: $name,
	  process: {
	    terminal: false,
	    user: { uid: $uid, gid: $gid },
	    cwd: (if (($c.WorkingDir // "")|length)>0 then $c.WorkingDir else "/" end),
	    args: $args,
	    env: $env,
	    capabilities: {
	      bounding: $caps, effective: $caps, inheritable: $caps,
	      permitted: $caps, ambient: []
	    },
	    rlimits: [ { type: "RLIMIT_NOFILE", hard: 1048576, soft: 1048576 } ],
	    noNewPrivileges: $nnp
	  },
	  root: { path: "rootfs", readonly: $roroot },
	  mounts: [
	    { destination: "/proc", type: "proc", source: "proc" },
	    { destination: "/sys",  type: "sysfs", source: "sysfs",
	      options: ["nosuid","noexec","nodev","ro"] },
	    { destination: "/tmp",  type: "tmpfs", source: "tmpfs",
	      options: ["nosuid","nodev","mode=1777"] },
	    { destination: "/run",  type: "tmpfs", source: "tmpfs",
	      options: ["nosuid","nodev","mode=0755"] }
	  ] + $resolv,
	  linux: ({ namespaces: $ns } + $res)
	}' > "$WORK/config.json" || die "failed to build config.json"

# --------------------------------------------------------------- apply profile
deepmerge_prog='
def deepmerge($a;$b):
  if ($a|type)=="object" and ($b|type)=="object" then
    reduce ($b|keys_unsorted[]) as $k ($a;
      .[$k] = (if (.[$k]==null) then $b[$k] else deepmerge(.[$k];$b[$k]) end))
  elif ($a|type)=="array" and ($b|type)=="array" then ($a+$b)
  else $b end;
deepmerge($base[0];$ovl[0])
| walk(if type=="object" then with_entries(select(.key|startswith("_")|not)) else . end)'

if [ -n "$PROFILE" ]; then
	PF="$PROFILE_DIR/$PROFILE.json"
	[ -f "$PF" ] || die "profile not found: $PF"
	log "==> applying profile: $PROFILE"
	jq -n --slurpfile base "$WORK/config.json" --slurpfile ovl "$PF" "$deepmerge_prog" \
		> "$WORK/config.merged.json" || die "profile merge failed"
	mv "$WORK/config.merged.json" "$WORK/config.json"
fi

# ------------------------------------------------------- network snippet (opt) -
# Emit, but never apply, an /etc/config/network stanza for an isolated container.
# Model: a veth pair; host end bridged, peer handed to the container netns and
# managed by uxc/netifd via proto 'infra'. Based on the OpenWrt container-veth
# pattern. Review and edit before applying. See docs/uxc-networking.md.
write_netconfig() {
	_host="vh-$NAME"; _peer="${NAME}0"
	# keep interface/device names within Linux's 15-char limit
	_host=$(printf '%.15s' "$_host"); _peer=$(printf '%.15s' "$_peer")
	cat > "$OUT/network.uci" <<EOF
# docker2uxc: /etc/config/network snippet for isolated container '$NAME'.
# NOT applied automatically - review, edit the bridge/addressing, then apply with:
#     cat network.uci >> /etc/config/network && /etc/init.d/network reload
# See docs/uxc-networking.md for the full explanation.

# 1) veth pair: host side '$_host', container side '$_peer'
config device
	option type 'veth'
	option name '$_host'
	option peer_name '$_peer'

# 2) attach the host side to a bridge (default '$NET_BRIDGE' - change if needed)
config device
	option name '$NET_BRIDGE'
	list ports '$_host'

# 3) container-side interface, managed inside the netns by uxc/netifd.
#    proto 'infra' lets uxc move '$_peer' into the container and wire DNS.
config interface '$NAME'
	option proto 'infra'
	option device '$_peer'
EOF
	log "    network: wrote $OUT/network.uci (isolated mode - review before applying)"
}

# ----------------------------------------------------- uxc keeper service -----
# uxc has no restart policy (Docker's "restart: unless-stopped"): 'uxc enable'
# only starts on boot, it does not respawn a container that exits. Apps like
# Frigate exit on purpose to reload config (web UI "Save & Restart") and expect
# the runtime to bring them back. This emits a small procd "keeper" service that
# does exactly that WHILE keeping the container managed by uxc (still shows in
# 'uxc list', controllable with uxc state/kill/attach) - it just re-runs
# 'uxc start' whenever the container is not running.
write_initd() {
	_bundle=$(cd "$OUT" && pwd)
	{
		echo '#!/bin/sh /etc/rc.common'
		echo "# uxc keeper for the docker2uxc bundle '$NAME' - gives uxc a Docker-style"
		echo "# auto-restart policy without bypassing uxc (stays in 'uxc list')."
		echo "# Install: cp $NAME.init /etc/init.d/$NAME-keeper && chmod +x /etc/init.d/$NAME-keeper"
		echo "#          /etc/init.d/$NAME-keeper enable && /etc/init.d/$NAME-keeper start"
		echo "# Stop with: /etc/init.d/$NAME-keeper stop  (plain 'uxc kill' would just be"
		echo "# restarted by the keeper, like Docker's restart: always)."
		echo 'USE_PROCD=1'
		echo 'START=95'
		echo 'STOP=10'
		echo "NAME=$NAME"
		echo "BUNDLE=$_bundle"
		cat <<'EOF'

start_service() {
	procd_open_instance "${NAME}-keeper"
	procd_set_param command /bin/sh -c "
		uxc create $NAME --bundle $BUNDLE 2>/dev/null
		while true; do
			st=\$(uxc state $NAME 2>/dev/null | jsonfilter -e '@.status' 2>/dev/null)
			if [ \"\$st\" != running ]; then
				# Recover: re-create (no --bundle - the path is stored, and
				# passing it again errors 'File exists'), then poll 'uxc start'
				# until it takes. 'uxc create' returns before the container is
				# start-ready, and how long that takes scales with image size,
				# so poll instead of a fixed sleep.
				uxc create $NAME 2>/dev/null
				n=0
				until uxc start $NAME 2>/dev/null || [ \$n -ge 60 ]; do n=\$((n+1)); sleep 1; done
			fi
			sleep 5
		done
	"
	procd_set_param respawn
	procd_close_instance
}

stop_service() {
	uxc kill "$NAME" 2>/dev/null
}
EOF
	} > "$OUT/$NAME.init"
	log "    init: wrote $OUT/$NAME.init (uxc keeper - auto-restart, stays uxc-managed)"
}

# ----------------------------------------------------------------- notes file --
write_notes() {
	{
		echo "# $NAME - generated by docker2uxc $VERSION"
		echo
		echo "Source image : $SOURCE ($ARCH)"
		echo "Config digest: $CONFIG_DIGEST"
		echo
		echo "## Entrypoint / Cmd"
		jq -r '.process.args | "    " + (map(@sh) | join(" "))' "$WORK/config.json"
		echo
		echo "## Network: $NETWORK"
		if [ "$NETWORK" = host ]; then
			echo "    Shares the host network namespace - no /etc/config/network setup needed."
		else
			echo "    Own network namespace. Apply the generated network.uci to"
			echo "    /etc/config/network (review it first) or DNS/connectivity will fail:"
			echo "        cat network.uci >> /etc/config/network && /etc/init.d/network reload"
		fi
		echo
		echo "## Exposed ports (from image - uxc does no port mapping)"
		jq -r '(.config.ExposedPorts // {}) | keys[] | "    " + .' "$CONFIG_BLOB" 2>/dev/null || true
		echo
		echo "## Volumes declared by image (MOUNT THESE BY HAND in config.json)"
		jq -r '(.config.Volumes // {}) | keys[] | "    " + .' "$CONFIG_BLOB" 2>/dev/null || true
		echo
		echo "Add bind mounts to config.json \"mounts\" like:"
		echo '    { "destination": "/data", "type": "bind",'
		echo '      "source": "/srv/'"$NAME"'/data", "options": ["rbind","rw"] }'
		echo
		echo "## Install (option A: uxc)"
		echo "    uxc create $NAME --bundle $(cd "$OUT" 2>/dev/null && pwd || echo "$OUT")${RW_OVERLAY:+ }$([ "$RW_OVERLAY" -eq 1 ] && echo "--write-overlay-path <overlay-dir>")"
		echo "    uxc start  $NAME"
		echo "    uxc enable $NAME      # autostart on boot"
		echo "    NOTE: 'uxc start' may print a cosmetic 'No such file or directory';"
		echo "    the container boots asynchronously - check 'uxc state $NAME'."
		echo
		if [ "$EMIT_KEEPER" -eq 1 ]; then
		echo "## Install (option B: uxc keeper - auto-restart, stays uxc-managed)"
		echo "    Use this if the app restarts itself (e.g. Frigate UI 'Save & Restart');"
		echo "    uxc has no respawn policy, so the plain uxc method won't come back."
		echo "    The keeper still leaves the container in 'uxc list' / uxc state|kill|attach."
		echo "    cp $NAME.init /etc/init.d/$NAME-keeper && chmod +x /etc/init.d/$NAME-keeper"
		echo "    /etc/init.d/$NAME-keeper enable && /etc/init.d/$NAME-keeper start"
		fi
	} > "$OUT/README.notes"
}

# -------------------------------------------------------------- write output --
# On overwrite keep the previous bundle as <out>.prev (one generation) so the
# container can be rolled back after an update (uxc rollback).
if [ -e "$OUT" ]; then
	[ "$FORCE" -eq 1 ] || die "output exists: $OUT (use --force)"
	rm -rf "$OUT.prev"
	mv "$OUT" "$OUT.prev"
fi
mkdir -p "$OUT"
mv "$ROOTFS" "$OUT/rootfs"
cp "$WORK/config.json"   "$OUT/config.json"
cp "$WORK/manifest.json" "$OUT/manifest.json"
cp "$CONFIG_BLOB"        "$OUT/image-config.json"
[ "$EMIT_NET" -eq 1 ] && write_netconfig
[ "$EMIT_KEEPER" -eq 1 ] && write_initd
write_notes

# Warn about bind sources that don't exist on the host: ujail stat()s every bind
# source and fails the WHOLE container if even one is missing (e.g. /etc/localtime
# is usually absent on OpenWrt). Warn, don't fail - the user may create them later.
MISSING=$(jq -r '.mounts[] | select(.type=="bind") | .source' "$OUT/config.json" \
	| while IFS= read -r s; do [ -e "$s" ] || printf '%s ' "$s"; done)
if [ -n "$MISSING" ]; then
	log ""
	log "WARNING: these bind mount sources do not exist on the host yet:"
	for s in $MISSING; do log "    $s"; done
	log "  Create them (mkdir -p ...) before 'uxc start', or ujail will refuse to"
	log "  build the container ('jail: mount_all() failed')."
fi

ABS_OUT=$(cd "$OUT" && pwd)

# ---------------------------------------------------------------- register --
# Write the uxcd registry entry $UXC_DIR/<name>.json directly (so it works even
# when uxcd is not running; uxcd reads the registry on demand). Per-container
# runtime overrides (volumes/devices/env/resources) are added here later by the
# user and survive image re-pulls. The bundle itself stays the image's config.
if [ "$REGISTER" -eq 1 ]; then
	mkdir -p "$UXC_DIR"
	_reg="$UXC_DIR/$NAME.json"
	# Overlay of the fields this (re-)registration sets. autostart/infra are
	# included only when given as flags, so a re-pull preserves the existing ones.
	jq -n \
		--arg name "$NAME" \
		--arg path "$ABS_OUT" \
		--arg image "$PROV_IMAGE" \
		--arg digest "$PROV_DIGEST" \
		--arg infra "$INFRA" \
		--argjson autostart "$([ "$AUTOSTART" -eq 1 ] && echo true || echo false)" \
		'{name:$name, path:$path}
		 + (if $image  != "" then {image:$image}   else {} end)
		 + (if $digest != "" then {digest:$digest} else {} end)
		 + (if $infra  != "" then {infra:$infra}   else {} end)
		 + (if $autostart    then {autostart:true} else {} end)' > "$_reg.new"
	# Merge into an existing entry so user overrides (volumes/devices/env/...) and
	# any autostart/infra survive a re-pull/upgrade; otherwise write it fresh.
	if [ -f "$_reg" ] && jq -e . "$_reg" >/dev/null 2>&1; then
		jq -s '.[0] + .[1]' "$_reg" "$_reg.new" > "$_reg.tmp" && mv "$_reg.tmp" "$_reg" && rm -f "$_reg.new"
	else
		mv "$_reg.new" "$_reg"
	fi
	log ""
	log "==> registered: $_reg"
fi

log ""
log "==> done: $OUT"
log "    rootfs: $(du -sh "$OUT/rootfs" 2>/dev/null | cut -f1)"
log ""
if [ "$REGISTER" -eq 1 ]; then
	log "Next:"
	log "    uxc start $NAME"
	log "    (add volumes/devices/env to $UXC_DIR/$NAME.json; see $OUT/README.notes)"
else
	log "Next:"
	log "    uxc create $NAME --bundle $ABS_OUT"
	log "    uxc start  $NAME"
	log "    (see $OUT/README.notes for ports, volumes and device mounts to add)"
fi
