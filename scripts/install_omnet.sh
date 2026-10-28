#!/usr/bin/env bash
# Install the official OMNeT++ core locally, with a headless runtime.
set -eo pipefail

project_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tool_root="${OMNET_TOOL_ROOT:-${project_root}/.tools}"
version=6.0.2
archive="${tool_root}/omnetpp-${version}-core.tgz"
install_dir="${tool_root}/omnetpp-${version}"
url="https://github.com/omnetpp/omnetpp/releases/download/omnetpp-${version}/omnetpp-${version}-core.tgz"
expected_md5=28822b9210e57b946f8bd09c701b855d

for command in curl md5sum tar make g++ bison flex; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing ${command}. On Ubuntu: sudo apt-get install build-essential bison flex curl" >&2
        exit 1
    fi
done

mkdir -p "$tool_root"
if [[ ! -f "${install_dir}/configure" ]]; then
    if [[ ! -f "$archive" ]]; then
        curl --fail --location --retry 3 --output "${archive}.part" "$url"
        echo "${expected_md5}  ${archive}.part" | md5sum --check --status || {
            echo "Downloaded OMNeT++ archive checksum failed: ${archive}.part" >&2
            exit 1
        }
        mv "${archive}.part" "$archive"
    fi
    echo "${expected_md5}  ${archive}" | md5sum --check --status || {
        echo "OMNeT++ archive checksum failed: ${archive}" >&2
        exit 1
    }
    tar -xzf "$archive" -C "$tool_root"
fi

# The upstream setup script reads environment variables that may be unset.
cd "$install_dir"
sed -i -e 's/^WITH_QTENV=.*/WITH_QTENV=no/' \
       -e 's/^WITH_OSG=.*/WITH_OSG=no/' \
       -e 's/^PREFER_CLANG=.*/PREFER_CLANG=no/' configure.user
source ./setenv -q
./configure
make -j"${OMNET_BUILD_JOBS:-$(nproc)}" MODE=release

echo "OMNeT++ ${version} installed at ${install_dir}"
echo "Activate it with: source \"${install_dir}/setenv\""
