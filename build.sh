#!/bin/bash

set -e
set -o pipefail

SCRIPT_LOCATION=$(dirname "$0")
MDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION"/../../../)
DUCKDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION")
BUILD_PATH=$(realpath "$MDB_SOURCE_PATH"/../DuckdbBuildOf_$(basename "$MDB_SOURCE_PATH"))
CPUS=$(getconf _NPROCESSORS_ONLN)
BUILD_TYPE_OPTIONS=("Debug" "RelWithDebInfo")
BUILD_TYPE="${BUILD_TYPE:-}"
DISTRO_OPTIONS=("ubuntu:22.04" "ubuntu:24.04" "debian:12" "rockylinux:8" "rockylinux:9")
DEFAULT_MDB_DATADIR="/var/lib/mysql"
USER="mysql"
GROUP="mysql"
INSTALL_PREFIX="/usr/"

usage() {
    echo "Usage: $0 [options]"
    echo "  -t <type>   Build type: ${BUILD_TYPE_OPTIONS[*]} (interactive if omitted)"
    echo "  -d <distro> Distro: ${DISTRO_OPTIONS[*]} (auto-detected if omitted)"
    echo "  -j <N>      Number of parallel jobs (default: $CPUS)"
    echo "  -c          CI mode: only build, skip install"
    echo "  -p          Build packages (DEB or RPM)"
    echo "  -S          Start MariaDB after build"
    echo "  -n          No clean: keep existing data files"
    echo "  -h          Show this help"
    exit 0
}

CI_MODE=false
START_MDB=false
NO_CLEAN=false
BUILD_PACKAGES=false
OS=""

while getopts "t:d:j:cpSnh" opt; do
    case $opt in
        t) BUILD_TYPE="$OPTARG" ;;
        d) OS="$OPTARG" ;;
        j) CPUS="$OPTARG" ;;
        c) CI_MODE=true ;;
        p) BUILD_PACKAGES=true ;;
        S) START_MDB=true ;;
        n) NO_CLEAN=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [[ ! " ${BUILD_TYPE_OPTIONS[*]} " =~ " ${BUILD_TYPE} " ]]; then
    echo "Select build type:"
    select BUILD_TYPE in "${BUILD_TYPE_OPTIONS[@]}"; do
        if [[ -n "$BUILD_TYPE" ]]; then
            break
        fi
        echo "Invalid selection, try again."
    done
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
        echo "!!!! Cannot detect distro, specify with -d !!!!"
        exit 1
    fi
    echo "  Detected distro: $OS"
}

select_pkg_format() {
    if [[ "$1" == *rocky* ]]; then
        PKG_FORMAT="rpm"
    else
        PKG_FORMAT="deb"
    fi
}

if [[ $BUILD_PACKAGES = true ]]; then
    if [[ ! " ${DISTRO_OPTIONS[*]} " =~ " ${OS} " ]]; then
        if [[ -z "$OS" ]]; then
            echo "Distro not specified, detecting..."
            detect_distro
        fi
        if [[ ! " ${DISTRO_OPTIONS[*]} " =~ " ${OS} " ]]; then
            echo "Select distro:"
            select OS in "${DISTRO_OPTIONS[@]}"; do
                if [[ -n "$OS" ]]; then
                    break
                fi
                echo "Invalid selection, try again."
            done
        fi
    fi
    select_pkg_format "$OS"
fi

echo "=== DuckDB Storage Engine Build ==="
echo "  Source:     $MDB_SOURCE_PATH"
echo "  Build dir:  $BUILD_PATH"
echo "  Build type: $BUILD_TYPE"
echo "  Jobs:       $CPUS"
if [[ $BUILD_PACKAGES = true ]]; then
    echo "  Packages:   $PKG_FORMAT ($OS)"
fi
echo ""

check_user_and_group() {
    local user=$1
    if [ -z "$(grep "$user" /etc/passwd)" ]; then
        echo "--- Adding user $user ---"
        useradd -r -U "$user" -d /var/lib/mysql
    fi
    if [ -z "$(grep "$user" /etc/group)" ]; then
        local gid=$(awk -F: '{uid[$3]=1}END{for(x=100; x<=999; x++) {if(uid[x] != ""){}else{print x; exit;}}}' /etc/group)
        echo "--- Adding group $user with id $gid ---"
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
    echo "--- Bootstrap MariaDB ---"
    "$INSTALL_PREFIX/bin/mariadb-install-db" \
        --datadir="$DEFAULT_MDB_DATADIR" \
        --user="$USER" --group="$GROUP" > /dev/null
}

stop_mdb() {
    if "$INSTALL_PREFIX/bin/mariadb-admin" ping --silent 2>/dev/null; then
        echo "--- Stopping MariaDB ---"
        "$INSTALL_PREFIX/bin/mariadb-admin" shutdown || true
    fi
}

start_mdb() {
    echo "--- Starting MariaDB ---"
    mkdir -p /run/mysqld
    chown "$USER:$GROUP" /run/mysqld
    "$INSTALL_PREFIX/bin/mariadbd-safe" --datadir="$DEFAULT_MDB_DATADIR" &

    local max_attempts=30
    local attempt=0
    while ! "$INSTALL_PREFIX/bin/mariadb-admin" ping --silent 2>/dev/null; do
        attempt=$((attempt + 1))
        if [[ $attempt -ge $max_attempts ]]; then
            echo "!!!! MariaDB failed to start within ${max_attempts} seconds !!!!"
            local err_log="${DEFAULT_MDB_DATADIR}/$(hostname).err"
            if [[ -f "$err_log" ]]; then
                echo "Last 50 lines of $err_log:"
                tail -50 "$err_log"
            fi
            exit 1
        fi
        sleep 1
    done
    echo "MariaDB is ready"
}

setup_dev_user() {
    local current_user=$(logname 2>/dev/null || echo "$SUDO_USER")
    if [[ -n "$current_user" && "$current_user" != "root" ]]; then
        echo "--- Creating dev user '$current_user' ---"
        "$INSTALL_PREFIX/bin/mariadb" -e \
            "CREATE USER IF NOT EXISTS '$current_user'@'localhost' IDENTIFIED VIA unix_socket;
             GRANT ALL PRIVILEGES ON *.* TO '$current_user'@'localhost';"
    fi
}

create_config() {
    # Put config in /etc/my.cnf.d/ which is included by /etc/my.cnf
    mkdir -p /etc/my.cnf.d
    cp "$DUCKDB_SOURCE_PATH/duckdb.cnf" /etc/my.cnf.d/duckdb.cnf
}

construct_cmake_flags() {
    MDB_CMAKE_FLAGS=(
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE
        -DCMAKE_INSTALL_PREFIX:PATH=$INSTALL_PREFIX
        -DCMAKE_EXPORT_COMPILE_COMMANDS=1
        -DPLUGIN_ARCHIVE=NO
        -DPLUGIN_BLACKHOLE=NO
        -DPLUGIN_FEDERATED=NO
        -DPLUGIN_FEDERATEDX=NO
        -DPLUGIN_CONNECT=NO
        -DPLUGIN_MROONGA=NO
        -DPLUGIN_OQGRAPH=NO
        -DPLUGIN_ROCKSDB=NO
        -DPLUGIN_SPHINX=NO
        -DPLUGIN_SPIDER=NO
        -DPLUGIN_TOKUDB=NO
        -DPLUGIN_COLUMNSTORE=NO
        -DWITH_EMBEDDED_SERVER=NO
        -DWITH_WSREP=NO
        -DWITH_SSL=system
        -DWITH_SAFEMALLOC=OFF
        -DMYSQL_MAINTAINER_MODE=OFF
        -DPLUGIN_DUCKDB=YES
        -DWITH_SBOM=0
        -DDBUG_ON=1
    )

    if [[ "$BUILD_TYPE" == "Debug" ]]; then
        MDB_CMAKE_FLAGS+=(-DDUCKDB_WERROR=ON)
    fi

    if [[ $BUILD_PACKAGES = true ]]; then
        if [[ "$PKG_FORMAT" == "rpm" ]]; then
            local os_version=${OS//[^0-9]/}
            if [[ "$OS" == *rocky* ]]; then
                MDB_CMAKE_FLAGS+=(-DRPM=rockylinux${os_version})
            fi
        else
            local codename=""
            case "$OS" in
                debian:12*)   codename="bookworm" ;;
                ubuntu:22.04) codename="jammy" ;;
                ubuntu:24.04) codename="noble" ;;
                *)            echo "!!!! Unknown DEB codename for $OS !!!!"; exit 1 ;;
            esac
            MDB_CMAKE_FLAGS+=(-DDEB=${codename} -DINSTALL_LAYOUT=DEB)
        fi
    else
        MDB_CMAKE_FLAGS+=(-DDEB=noble -DINSTALL_LAYOUT=DEB)
    fi
}

construct_cmake_flags

build_binary() {
    echo "--- Configuring ---"
    cmake "${MDB_CMAKE_FLAGS[@]}" -S"$MDB_SOURCE_PATH" -B"$BUILD_PATH"

    echo "--- Building ---"
    cmake --build "$BUILD_PATH" -j "$CPUS"

    if [ $? -ne 0 ]; then
        echo "!!!! BUILD FAILED !!!!"
        exit 1
    fi

    echo ""
    echo "--- Adding compile_commands.json symlink ---"
    ln -sf "$BUILD_PATH/compile_commands.json" "$MDB_SOURCE_PATH"
}

build_package() {
    echo "--- Building $PKG_FORMAT package for $OS ---"

    if [[ "$PKG_FORMAT" == "rpm" ]]; then
        cd "$BUILD_PATH"
        make -j "$CPUS" package
    else
        cd "$MDB_SOURCE_PATH"
        export DEBIAN_FRONTEND="noninteractive"
        export DEB_BUILD_OPTIONS="parallel=$CPUS"
        export BUILDPACKAGE_FLAGS="-b"
        CMAKEFLAGS="${MDB_CMAKE_FLAGS[*]}" debian/autobake-deb.sh
    fi

    if [ $? -ne 0 ]; then
        echo "!!!! PACKAGE BUILD FAILED !!!!"
        exit 1
    fi
    echo "--- Packages ready ---"
}

build_binary

if [[ $BUILD_PACKAGES = true ]]; then
    build_package
    echo "=== BUILD FINISHED ==="
    exit 0
fi

if [[ $CI_MODE = false ]]; then
    check_user_and_group "$USER"
    stop_mdb
    clean_old_installation

    echo "--- Installing ---"
    cmake --install "$BUILD_PATH"

    create_config
    if [[ $NO_CLEAN = false ]]; then
        bootstrap_mdb
    else
        echo "--- Skipping bootstrap (--no-clean mode, keeping existing data) ---"
    fi
fi

if [[ $START_MDB = true ]]; then
    stop_mdb
    start_mdb
    setup_dev_user

    echo "--- Registering DuckDB UDFs ---"
    "$INSTALL_PREFIX/bin/mariadb" < "$DUCKDB_SOURCE_PATH/scripts/install.sql"
fi

echo "=== BUILD FINISHED ==="
