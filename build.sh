#!/bin/bash

set -e
set -o pipefail

SCRIPT_LOCATION=$(dirname "$0")
MDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION"/../../)
DUCKDB_SOURCE_PATH=$(realpath "$SCRIPT_LOCATION")
BUILD_PATH=$(realpath "$MDB_SOURCE_PATH"/../DuckdbBuildOf_$(basename "$MDB_SOURCE_PATH"))
CPUS=$(getconf _NPROCESSORS_ONLN)
BUILD_TYPE="${BUILD_TYPE:-Debug}"
DEFAULT_MDB_DATADIR="/var/lib/mysql"
USER="mysql"
GROUP="mysql"
INSTALL_PREFIX="/usr/"

usage() {
    echo "Usage: $0 [options]"
    echo "  -t <type>   Build type: Debug|RelWithDebInfo (default: Debug)"
    echo "  -j <N>      Number of parallel jobs (default: $CPUS)"
    echo "  -c          CI mode: only build, skip install"
    echo "  -S          Start MariaDB after build"
    echo "  -n          No clean: keep existing data files"
    echo "  -h          Show this help"
    exit 0
}

CI_MODE=false
START_MDB=false
NO_CLEAN=false

while getopts "t:j:cSnh" opt; do
    case $opt in
        t) BUILD_TYPE="$OPTARG" ;;
        j) CPUS="$OPTARG" ;;
        c) CI_MODE=true ;;
        S) START_MDB=true ;;
        n) NO_CLEAN=true ;;
        h) usage ;;
        *) usage ;;
    esac
done

echo "=== DuckDB Storage Engine Build ==="
echo "  Source:     $MDB_SOURCE_PATH"
echo "  Build dir:  $BUILD_PATH"
echo "  Build type: $BUILD_TYPE"
echo "  Jobs:       $CPUS"
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
    -DDEB=noble
    -DINSTALL_LAYOUT=DEB
    -DDBUG_ON=1
)

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
fi

echo "=== BUILD FINISHED ==="
