#!/bin/bash

set -e
set -o pipefail

SCRIPT_LOCATION=$(dirname "$0")
source "$SCRIPT_LOCATION/utils.sh"
DUCKDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION")
BUILD_PATH="$DUCKDB_SOURCE_PATH/build"
MARIADB_PREFIX="/usr"
CPUS=$(getconf _NPROCESSORS_ONLN)
BUILD_TYPE_OPTIONS=("Debug" "RelWithDebInfo")
BUILD_TYPE="${BUILD_TYPE:-}"
PKG_FORMAT_OPTIONS=("rpm" "deb" "tgz")
LAYOUT_OPTIONS=("DEB" "RPM" "STANDALONE")
LAYOUT=""
DEFAULT_MDB_DATADIR="/var/lib/mysql"
USER="mysql"
GROUP="mysql"
GCC_VERSION="12"

usage() {
    echo "Usage: $0 [options]"
    echo "  Paths:"
    echo "  --mariadb-prefix <dir>  MariaDB installation with the Development component"
    echo "                          (default: $MARIADB_PREFIX). Used both as CMAKE_PREFIX_PATH"
    echo "                          and as the server prefix (bin/, sbin/, plugin dir)"
    echo "  --build-dir <dir>       Plugin build directory (default: $BUILD_PATH)"
    echo "  --datadir <dir>         Server data directory (default: $DEFAULT_MDB_DATADIR)"
    echo "  --layout <layout>       Install layout of the server: ${LAYOUT_OPTIONS[*]}"
    echo "                          (default: RPM for -p rpm, STANDALONE for -p tgz, otherwise DEB)"
    echo ""
    echo "  Build:"
    echo "  -t <type>   Build type: ${BUILD_TYPE_OPTIONS[*]} (interactive if omitted)"
    echo "  -j <N>      Number of parallel jobs (default: $CPUS)"
    echo "  -u          Build unit tests"
    echo "  -a          Enable ASAN/UBSAN (the server must be built the same way)"
    echo "  -p <fmt>    Build a package: ${PKG_FORMAT_OPTIONS[*]}"
    echo ""
    echo "  Run:"
    echo "  -c          CI mode: only build, skip install"
    echo "  -S          Start MariaDB after build"
    echo "  -n          No clean: keep existing data files"
    echo "  -D          Install build prerequisites (requires root/sudo)"
    echo "  -R          Use gcc-toolset-\${GCC_VERSION} on Rocky 8"
    echo "  -h          Show this help"
    exit 0
}

CI_MODE=false
START_MDB=false
NO_CLEAN=false
PKG_FORMAT=""
INSTALL_DEPS=false
GCC_TOOLSET=false
UNIT_TESTS=false
WITH_ASAN=false
OS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mariadb-prefix) MARIADB_PREFIX=$(realpath -m "$2"); shift ;;
        --mariadb-prefix=*) MARIADB_PREFIX=$(realpath -m "${1#*=}") ;;
        --build-dir) BUILD_PATH=$(realpath -m "$2"); shift ;;
        --build-dir=*) BUILD_PATH=$(realpath -m "${1#*=}") ;;
        --datadir) DEFAULT_MDB_DATADIR=$(realpath -m "$2"); shift ;;
        --datadir=*) DEFAULT_MDB_DATADIR=$(realpath -m "${1#*=}") ;;
        --layout) LAYOUT="$2"; shift ;;
        --layout=*) LAYOUT="${1#*=}" ;;
        -t) BUILD_TYPE="$2"; shift ;;
        -j) CPUS="$2"; shift ;;
        -p) PKG_FORMAT="$2"; shift ;;
        -c) CI_MODE=true ;;
        -S) START_MDB=true ;;
        -n) NO_CLEAN=true ;;
        -D) INSTALL_DEPS=true ;;
        -R) GCC_TOOLSET=true ;;
        -u) UNIT_TESTS=true ;;
        -a) WITH_ASAN=true ;;
        -h|--help) usage ;;
        *) warn "Unknown argument: $1"; usage ;;
    esac
    shift
done

if [[ ! " ${BUILD_TYPE_OPTIONS[*]} " =~ " ${BUILD_TYPE} " ]]; then
    menu_choice "Select build type:" BUILD_TYPE_OPTIONS
    BUILD_TYPE="$MENU_RESULT"
fi

if [[ -n "$PKG_FORMAT" && ! " ${PKG_FORMAT_OPTIONS[*]} " =~ " ${PKG_FORMAT} " ]]; then
    fail "Unsupported package format: $PKG_FORMAT (expected: ${PKG_FORMAT_OPTIONS[*]})"
fi

# The SDK does not record the server's layout and defaults to STANDALONE,
# which puts the plugin where a DEB/RPM server does not look for it.
if [[ -z "$LAYOUT" ]]; then
    case "$PKG_FORMAT" in
        rpm) LAYOUT="RPM" ;;
        tgz) LAYOUT="STANDALONE" ;;
        *)   LAYOUT="DEB" ;;
    esac
fi
if [[ ! " ${LAYOUT_OPTIONS[*]} " =~ " ${LAYOUT} " ]]; then
    fail "Unsupported layout: $LAYOUT (expected: ${LAYOUT_OPTIONS[*]})"
fi

detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        local os_name=$(echo "$NAME" | cut -f1 -d" " | tr '[:upper:]' '[:lower:]')
        OS="${os_name}:${VERSION_ID}"
    elif [ -f /etc/lsb-release ]; then
        . /etc/lsb-release
        OS=$(echo "$DISTRIB_ID" | tr '[:upper:]' '[:lower:]'):"$DISTRIB_RELEASE"
    else
        fail "Cannot detect distro"
    fi
    info "Detected distro: ${_CLR_YELLOW}$OS"
}

SDK_CONFIG="$MARIADB_PREFIX/share/cmake/mariadb-plugin/mariadb-plugin-config.cmake"

header "DuckDB Storage Engine Build"
info "Source:     ${_CLR_YELLOW}$DUCKDB_SOURCE_PATH"
info "MariaDB:    ${_CLR_YELLOW}$MARIADB_PREFIX"
info "Build dir:  ${_CLR_YELLOW}$BUILD_PATH"
info "Build type: ${_CLR_YELLOW}$BUILD_TYPE"
info "Layout:     ${_CLR_YELLOW}$LAYOUT"
info "Jobs:       ${_CLR_YELLOW}$CPUS"
info "ASAN/UBSAN: ${_CLR_YELLOW}$WITH_ASAN"
info "Unit tests: ${_CLR_YELLOW}$UNIT_TESTS"
if [[ -n "$PKG_FORMAT" ]]; then
    info "Package:    ${_CLR_YELLOW}$PKG_FORMAT"
fi
echo ""

check_user_and_group() {
    local user=$1
    if [ -z "$(grep "$user" /etc/passwd)" ]; then
        info "Adding user $user"
        useradd -r -U "$user" -d "$DEFAULT_MDB_DATADIR"
    fi
    if [ -z "$(grep "$user" /etc/group)" ]; then
        local gid=$(awk -F: '{uid[$3]=1}END{for(x=100; x<=999; x++) {if(uid[x] != ""){}else{print x; exit;}}}' /etc/group)
        info "Adding group $user with id $gid"
        groupadd -g "$gid" "$user"
    fi
}

clean_old_installation() {
    if [[ $NO_CLEAN = true ]]; then
        return
    fi
    rm -rf "${DEFAULT_MDB_DATADIR}"
    rm -rf /var/run/mysqld
}

bootstrap_mdb() {
    info "Bootstrap MariaDB"
    ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" "$MARIADB_PREFIX/bin/mariadb-install-db" \
        --datadir="$DEFAULT_MDB_DATADIR" \
        --user="$USER" --group="$GROUP" > /dev/null
}

stop_mdb() {
    if "$MARIADB_PREFIX/bin/mariadb-admin" ping --silent 2>/dev/null; then
        warn "Stopping MariaDB"
        "$MARIADB_PREFIX/bin/mariadb-admin" shutdown || true
    fi
}

start_mdb() {
    info "Starting MariaDB"
    mkdir -p /run/mysqld
    chown "$USER:$GROUP" /run/mysqld
    "$MARIADB_PREFIX/bin/mariadbd-safe" --datadir="$DEFAULT_MDB_DATADIR" &

    local max_attempts=30
    local attempt=0
    while ! "$MARIADB_PREFIX/bin/mariadb-admin" ping --silent 2>/dev/null; do
        attempt=$((attempt + 1))
        if [[ $attempt -ge $max_attempts ]]; then
            local err_log="${DEFAULT_MDB_DATADIR}/$(hostname).err"
            if [[ -f "$err_log" ]]; then
                error "Last 50 lines of $err_log:"
                tail -50 "$err_log"
            fi
            fail "MariaDB failed to start within ${max_attempts} seconds"
        fi
        sleep 1
    done
    success "MariaDB is ready"
}

setup_dev_user() {
    local current_user=$(logname 2>/dev/null || echo "$SUDO_USER")
    if [[ -n "$current_user" && "$current_user" != "root" ]]; then
        info "Creating dev user '${_CLR_YELLOW}$current_user${_CLR_CYAN}'"
        "$MARIADB_PREFIX/bin/mariadb" -e \
            "CREATE USER IF NOT EXISTS '$current_user'@'localhost' IDENTIFIED VIA unix_socket;
             GRANT ALL PRIVILEGES ON *.* TO '$current_user'@'localhost';"
    fi
}

check_sdk() {
    [[ -f "$SDK_CONFIG" ]] \
        || fail "No MariaDB plugin SDK under $MARIADB_PREFIX (install the Development component, MDEV-40608+)"
}

# The plugin is ABI-bound to the server it was built against: refuse to
# install into a server whose version differs from the SDK's.
check_server_version() {
    local mariadbd=""
    for f in "$MARIADB_PREFIX/sbin/mariadbd" "$MARIADB_PREFIX/bin/mariadbd"; do
        [[ -x "$f" ]] && { mariadbd="$f"; break; }
    done
    [[ -n "$mariadbd" ]] || fail "No MariaDB server under $MARIADB_PREFIX"

    local sdk_ver srv_ver
    sdk_ver=$(sed -n 's/^SET(SERVER_VERSION \(.*\))$/\1/p' "$SDK_CONFIG")
    srv_ver=$("$mariadbd" --version | sed -n 's/.* Ver \([0-9.]*\).*/\1/p')
    [[ "$sdk_ver" == "$srv_ver" ]] \
        || fail "SDK version $sdk_ver does not match server $mariadbd version $srv_ver"
    info "Server:     ${_CLR_YELLOW}$srv_ver ($mariadbd)"
}

construct_cmake_flags() {
    MDB_CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE
        "-DCMAKE_PREFIX_PATH=$MARIADB_PREFIX"
        -DINSTALL_LAYOUT=$LAYOUT
        -DCMAKE_EXPORT_COMPILE_COMMANDS=1
    )

    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        MDB_CMAKE_FLAGS+=(-DDUCKDB_WERROR=ON)
    fi

    if [[ $UNIT_TESTS = true ]]; then
        MDB_CMAKE_FLAGS+=(-DDUCKDB_UNIT_TESTS=ON)
    fi

    if [[ $WITH_ASAN = true ]]; then
        MDB_CMAKE_FLAGS+=(-DWITH_ASAN=ON -DWITH_UBSAN=ON)
    fi

    case "$PKG_FORMAT" in
        rpm) MDB_CMAKE_FLAGS+=(-DRPM=1) ;;
        deb) MDB_CMAKE_FLAGS+=(-DDEB=1) ;;
    esac
}

install_deps() {
    if [[ $INSTALL_DEPS = false ]]; then
        return
    fi

    if [[ -z "$OS" ]]; then
        detect_distro
    fi

    local SUDO=""
    if [[ $EUID -ne 0 ]]; then
        SUDO="sudo"
    fi

    # Plugin build prerequisites. The MariaDB devel package must provide
    # the plugin SDK (share/cmake/mariadb-plugin, MDEV-40608+).
    local RPM_DEPS="git make cmake gcc gcc-c++ libatomic MariaDB-devel"
    if [[ "$PKG_FORMAT" == "rpm" ]]; then
        RPM_DEPS="$RPM_DEPS rpm-build"
    fi

    local DEB_DEPS="build-essential git cmake libmariadb-dev"
    if [[ "$PKG_FORMAT" == "deb" ]]; then
        DEB_DEPS="$DEB_DEPS lsb-release"
    fi

    local command=""
    case "$OS" in
        rockylinux:8|rocky:8)
            command="$SUDO dnf install -y ${RPM_DEPS}"
            if [[ $GCC_TOOLSET = true ]]; then
                command="$command && $SUDO dnf install -y gcc-toolset-${GCC_VERSION} gcc-toolset-${GCC_VERSION}-gcc-c++"
                warn "Activate toolchain before rebuilding: . /opt/rh/gcc-toolset-${GCC_VERSION}/enable"
            else
                warn "Rocky 8 default gcc 8 lacks C++20 -- consider re-running with -R"
            fi
            ;;
        rockylinux:9|rocky:9|rocky:10|rhel:9*|redhat:9*|red:9*)
            command="$SUDO dnf install -y ${RPM_DEPS}"
            ;;
        ubuntu:*|debian:*)
            command="$SUDO apt-get update && \
                     DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y ${DEB_DEPS}"
            ;;
        *)
            fail "Unsupported distro for -D: $OS"
            ;;
    esac

    separator
    info "Installing prerequisites for ${_CLR_YELLOW}$OS"
    set +e
    eval "$command" | one_liner
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "DEPENDENCY INSTALL FAILED (exit code $rc)"
    success "Prerequisites installed"
}

construct_cmake_flags
install_deps
check_sdk

build_binary() {
    separator
    info "Configuring"
    set +e
    cmake "${MDB_CMAKE_FLAGS[@]}" -S"$DUCKDB_SOURCE_PATH" -B"$BUILD_PATH" | one_liner
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "CONFIGURE FAILED (exit code $rc)"

    separator
    info "Building with ${_CLR_YELLOW}$CPUS${_CLR_CYAN} jobs"
    set +e
    cmake --build "$BUILD_PATH" -j "$CPUS" | one_liner
    rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "BUILD FAILED (exit code $rc)"

    success "Build complete"
    info "Adding compile_commands.json symlink"
    ln -sf "$BUILD_PATH/compile_commands.json" "$DUCKDB_SOURCE_PATH"
}

build_package() {
    separator
    info "Building ${_CLR_YELLOW}$PKG_FORMAT${_CLR_CYAN} package"
    set +e
    cmake --build "$BUILD_PATH" -j "$CPUS" --target package | one_liner
    local rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "PACKAGE BUILD FAILED (exit code $rc)"
    success "Packages ready"
}

build_binary

if [[ -n "$PKG_FORMAT" ]]; then
    build_package
    header "BUILD FINISHED"
    exit 0
fi

if [[ $CI_MODE = false ]]; then
    check_server_version
    check_user_and_group "$USER"
    stop_mdb
    clean_old_installation

    separator
    info "Installing"
    set +e
    cmake --install "$BUILD_PATH" | one_liner
    rc=${PIPESTATUS[0]}
    set -e
    [[ $rc -ne 0 ]] && fail "INSTALL FAILED (exit code $rc)"

    if [[ $NO_CLEAN = false ]]; then
        bootstrap_mdb
    else
        warn "Skipping bootstrap (--no-clean mode, keeping existing data)"
    fi
fi

if [[ $START_MDB = true ]]; then
    stop_mdb
    start_mdb
    setup_dev_user

fi

header "BUILD FINISHED"
