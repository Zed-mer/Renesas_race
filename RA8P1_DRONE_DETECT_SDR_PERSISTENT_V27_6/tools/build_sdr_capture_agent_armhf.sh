#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "${script_dir}/.." && pwd)"
out_dir="${1:-${repo_dir}/tmp/build_capture_agent_armhf}"
cc="${CC:-arm-linux-gnueabihf-gcc}"
readelf_tool="${READELF:-arm-linux-gnueabihf-readelf}"
uuencode_tool="${UUENCODE:-uuencode}"
uudecode_tool="${UUDECODE:-uudecode}"
sha256_tool="${SHA256SUM:-sha256sum}"
host_cc="${HOST_CC:-gcc}"
build_iio_mmap_adapter="${BUILD_IIO_MMAP_ADAPTER:-1}"

case "${build_iio_mmap_adapter}" in
    0|1)
        ;;
    *)
        echo "BUILD_IIO_MMAP_ADAPTER must be 0 or 1" >&2
        exit 2
        ;;
esac

mkdir -p "${out_dir}"

cleanup()
{
    rm -f \
        "${out_dir}/libdl.so.2" \
        "${out_dir}/libpthread.so.0" \
        "${out_dir}/sdr_glibc_225_compat.o" \
        "${out_dir}/sdr_capture_agent.uue.tmp" \
        "${out_dir}/sdr_capture_agent.uue.verify" \
        "${out_dir}/sdr_adapter_libiio.so.uue.tmp" \
        "${out_dir}/sdr_adapter_libiio.so.uue.verify" \
        "${out_dir}/sdr_adapter_iio_mmap.so.uue.tmp" \
        "${out_dir}/sdr_adapter_iio_mmap.so.uue.verify" \
        "${out_dir}/sdr_adapter_iio_mmap.host.so" \
        "${out_dir}/test_sdr_iio_mmap_abi.host" \
        "${out_dir}/test_sdr_iio_mmap_copy.host"
}
trap cleanup EXIT
cleanup

common_flags=(
    -std=c11
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -pthread
    -march=armv7-a
    -mfpu=neon
    -mfloat-abi=hard
    -marm
    -U_TIME_BITS
    -D_TIME_BITS=32
    -U_FILE_OFFSET_BITS
    -D_FILE_OFFSET_BITS=32
    -U_FORTIFY_SOURCE
    -D_FORTIFY_SOURCE=0
    -fno-stack-protector
)

host_flags=(
    -std=c11
    -O2
    -Wall
    -Wextra
    -Wpedantic
    -Werror
)

"${cc}" -shared -nostdlib -fPIC \
    -Wl,-soname,libdl.so.2 \
    -Wl,--version-script,"${script_dir}/sdr_libdl_glibc_225.map" \
    "${script_dir}/sdr_libdl_glibc_225_import.c" \
    -o "${out_dir}/libdl.so.2"

"${cc}" -shared -nostdlib -fPIC \
    -Wl,-soname,libpthread.so.0 \
    -Wl,--version-script,"${script_dir}/sdr_libpthread_glibc_225.map" \
    "${script_dir}/sdr_libpthread_glibc_225_import.c" \
    -o "${out_dir}/libpthread.so.0"

"${cc}" "${common_flags[@]}" -c \
    "${script_dir}/sdr_glibc_225_compat.c" \
    -o "${out_dir}/sdr_glibc_225_compat.o"

"${cc}" "${common_flags[@]}" -pthread \
    -Dfcntl=sdr_fcntl_glibc_2_4 -fno-pie -no-pie \
    "${script_dir}/sdr_capture_agent.c" \
    "${out_dir}/sdr_glibc_225_compat.o" \
    -Wl,--wrap=__libc_start_main \
    -Wl,--wrap=__isoc23_strtoul \
    -Wl,--wrap=__isoc23_strtoull \
    -Wl,--no-as-needed \
    "${out_dir}/libdl.so.2" \
    "${out_dir}/libpthread.so.0" \
    -Wl,--as-needed \
    -o "${out_dir}/sdr_capture_agent"

"${cc}" "${common_flags[@]}" -shared -fPIC \
    "${script_dir}/sdr_adapter_libiio.c" \
    -Wl,--no-as-needed "${out_dir}/libdl.so.2" -Wl,--as-needed \
    -o "${out_dir}/sdr_adapter_libiio.so"

artifacts=(
    "${out_dir}/sdr_capture_agent" \
    "${out_dir}/sdr_adapter_libiio.so"
)
libdl_artifacts=(
    "${out_dir}/sdr_capture_agent"
    "${out_dir}/sdr_adapter_libiio.so"
)

if [[ "${build_iio_mmap_adapter}" == "1" ]]; then
    "${cc}" "${common_flags[@]}" -shared -fPIC \
        "${script_dir}/sdr_adapter_iio_mmap.c" \
        "${out_dir}/sdr_glibc_225_compat.o" \
        -Wl,--wrap=__isoc23_strtoul \
        -o "${out_dir}/sdr_adapter_iio_mmap.so"
    artifacts+=("${out_dir}/sdr_adapter_iio_mmap.so")

    # This verifies only the plugin ABI table on the build host.  It does not
    # open IIO devices or claim that the Pluto block/mmap path works on board.
    "${host_cc}" "${host_flags[@]}" -shared -fPIC \
        "${script_dir}/sdr_adapter_iio_mmap.c" \
        -o "${out_dir}/sdr_adapter_iio_mmap.host.so"
    "${host_cc}" "${host_flags[@]}" \
        "${script_dir}/test_sdr_iio_mmap_abi.c" \
        -ldl \
        -o "${out_dir}/test_sdr_iio_mmap_abi.host"
    "${out_dir}/test_sdr_iio_mmap_abi.host" \
        "${out_dir}/sdr_adapter_iio_mmap.host.so"
    "${host_cc}" "${host_flags[@]}" -DRA8P1_IIO_MMAP_TESTING \
        "${script_dir}/sdr_adapter_iio_mmap.c" \
        "${script_dir}/test_sdr_iio_mmap_copy.c" \
        -pthread \
        -o "${out_dir}/test_sdr_iio_mmap_copy.host"
    "${out_dir}/test_sdr_iio_mmap_copy.host"
else
    # Do not leave a previously built experimental plugin available for an
    # operator who deliberately disabled it in this build.
    rm -f \
        "${out_dir}/sdr_adapter_iio_mmap.so" \
        "${out_dir}/sdr_adapter_iio_mmap.so.uue"
fi

for artifact in "${artifacts[@]}"
do
    elf_description="$(${readelf_tool} -h -A "${artifact}")"
    if ! grep -q 'Class:.*ELF32' <<<"${elf_description}" || \
       ! grep -q 'Machine:.*ARM' <<<"${elf_description}" || \
       ! grep -q 'Flags:.*hard-float ABI' <<<"${elf_description}" || \
       ! grep -q 'Tag_ABI_VFP_args: VFP registers' <<<"${elf_description}"
    then
        echo "${artifact}: is not an ARM EABI hard-float artifact" >&2
        exit 1
    fi

    bad_versions="$(${readelf_tool} --version-info "${artifact}" | \
        sed -n 's/.*Name: \(GLIBC_[0-9][0-9.]*\).*/\1/p' | \
        sort -Vu | \
        awk -F'[_.]' '($2 > 2) || (($2 == 2) && ($3 > 25))')"
    if [[ -n "${bad_versions}" ]]; then
        echo "${artifact}: requires unsupported ${bad_versions}" >&2
        exit 1
    fi

done

for artifact in "${libdl_artifacts[@]}"
do
    if ! "${readelf_tool}" -d "${artifact}" | \
        grep -q 'Shared library: \[libdl.so.2\]'
    then
        echo "${artifact}: does not depend on target libdl.so.2" >&2
        exit 1
    fi
done

if [[ "${build_iio_mmap_adapter}" == "1" ]] && \
   ! "${readelf_tool}" --wide --dyn-syms \
       "${out_dir}/sdr_adapter_iio_mmap.so" | \
       grep -q "${RA8P1_SDR_ADAPTER_GET_API_SYMBOL:-ra8p1_sdr_adapter_get_api_v1}"
then
    echo "${out_dir}/sdr_adapter_iio_mmap.so: adapter API symbol is not exported" >&2
    exit 1
fi

if ! "${readelf_tool}" -d "${out_dir}/sdr_capture_agent" | \
    grep -q 'Shared library: \[libpthread.so.0\]'
then
    echo "${out_dir}/sdr_capture_agent: does not depend on target libpthread.so.0" >&2
    exit 1
fi

if command -v "${uuencode_tool}" >/dev/null 2>&1 && \
   command -v "${uudecode_tool}" >/dev/null 2>&1
then
    for artifact in "${artifacts[@]}"
    do
        wrapper="${artifact}.uue"
        wrapper_tmp="${wrapper}.tmp"
        decoded="${artifact}.uue.verify"
        "${uuencode_tool}" "${artifact}" "$(basename "${artifact}")" >"${wrapper_tmp}"
        "${uudecode_tool}" -o "${decoded}" "${wrapper_tmp}"
        cmp "${artifact}" "${decoded}"
        rm -f "${decoded}"
        mv -f "${wrapper_tmp}" "${wrapper}"
    done
else
    # A successful ELF rebuild must not leave a wrapper for an older ELF.
    for artifact in "${artifacts[@]}"
    do
        rm -f "${artifact}.uue"
    done
    echo "warning: uuencode/uudecode unavailable; no .uue wrappers generated" >&2
fi

"${readelf_tool}" -h "${out_dir}/sdr_capture_agent" | \
    grep -E 'Class:|Data:|Machine:|Type:'
"${readelf_tool}" --version-info "${out_dir}/sdr_capture_agent" | \
    sed -n 's/.*Name: \(GLIBC_[0-9][0-9.]*\).*/\1/p' | sort -Vu
echo "built ${out_dir}/sdr_capture_agent"
echo "built ${out_dir}/sdr_adapter_libiio.so"
if [[ "${build_iio_mmap_adapter}" == "1" ]]; then
    echo "built ${out_dir}/sdr_adapter_iio_mmap.so (experimental; ABI-only host check)"
fi
"${sha256_tool}" "${artifacts[@]}"
