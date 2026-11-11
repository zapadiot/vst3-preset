#!/usr/bin/env bash
set -euo pipefail

# Build vstparam_host against the Steinberg VST3 SDK sources.
# Usage:
#   VST3_SDK=/path/to/vst3sdk ./build.sh [output_path]
#   ./build.sh /path/to/vst3sdk [output_path]
#
# Requirements: g++ (C++17), pthread, dl

if [[ -n "${VST3_SDK:-}" ]]; then
  B="$VST3_SDK"
elif [[ $# -ge 1 && -d "${1:-}" ]]; then
  B="$1"; shift
else
  echo "error: set VST3_SDK or pass the VST3 SDK path as the first argument" >&2
  echo "usage: VST3_SDK=/path/to/vst3sdk ./build.sh [output_path]" >&2
  exit 2
fi

OUT="${1:-$(dirname "$0")/vstparam_host}"
SRC="$(dirname "$0")/source/vstparam_host.cpp"
CXX="${CXX:-c++}"
FLAGS="-I$B -Wno-multichar -O2 -DNDEBUG -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -DSMTG_RENAME_ASSERT=1 -DRELEASE=1"

"$CXX" $FLAGS -o "$OUT" \
  "$SRC" \
  "$B/base/source/baseiids.cpp" \
  "$B/base/source/fbuffer.cpp" \
  "$B/base/source/fdebug.cpp" \
  "$B/base/source/fdynlib.cpp" \
  "$B/base/source/fobject.cpp" \
  "$B/base/source/fstreamer.cpp" \
  "$B/base/source/fstring.cpp" \
  "$B/base/source/timer.cpp" \
  "$B/base/source/updatehandler.cpp" \
  "$B/base/thread/source/fcondition.cpp" \
  "$B/base/thread/source/flock.cpp" \
  "$B/pluginterfaces/base/conststringtable.cpp" \
  "$B/pluginterfaces/base/coreiids.cpp" \
  "$B/pluginterfaces/base/funknown.cpp" \
  "$B/pluginterfaces/base/ustring.cpp" \
  "$B/public.sdk/source/common/commoniids.cpp" \
  "$B/public.sdk/source/common/commonstringconvert.cpp" \
  "$B/public.sdk/source/common/memorystream.cpp" \
  "$B/public.sdk/source/common/openurl.cpp" \
  "$B/public.sdk/source/common/pluginview.cpp" \
  "$B/public.sdk/source/common/readfile.cpp" \
  "$B/public.sdk/source/common/systemclipboard_linux.cpp" \
  "$B/public.sdk/source/common/threadchecker_linux.cpp" \
  "$B/public.sdk/source/main/moduleinit.cpp" \
  "$B/public.sdk/source/main/linuxmain.cpp" \
  "$B/public.sdk/source/main/pluginfactory.cpp" \
  "$B/public.sdk/source/vst/vstaudioeffect.cpp" \
  "$B/public.sdk/source/vst/vstbus.cpp" \
  "$B/public.sdk/source/vst/vstcomponent.cpp" \
  "$B/public.sdk/source/vst/vstcomponentbase.cpp" \
  "$B/public.sdk/source/vst/vsteditcontroller.cpp" \
  "$B/public.sdk/source/vst/vstinitiids.cpp" \
  "$B/public.sdk/source/vst/vstnoteexpressiontypes.cpp" \
  "$B/public.sdk/source/vst/vstparameters.cpp" \
  "$B/public.sdk/source/vst/vstrepresentation.cpp" \
  "$B/public.sdk/source/vst/utility/dataexchange.cpp" \
  "$B/public.sdk/source/vst/utility/stringconvert.cpp" \
  "$B/public.sdk/source/vst/utility/systemtime.cpp" \
  "$B/public.sdk/source/vst/hosting/connectionproxy.cpp" \
  "$B/public.sdk/source/vst/hosting/eventlist.cpp" \
  "$B/public.sdk/source/vst/hosting/hostclasses.cpp" \
  "$B/public.sdk/source/vst/hosting/hostdataexchangehandler.cpp" \
  "$B/public.sdk/source/vst/hosting/module.cpp" \
  "$B/public.sdk/source/vst/hosting/module_linux.cpp" \
  "$B/public.sdk/source/vst/hosting/parameterchanges.cpp" \
  "$B/public.sdk/source/vst/hosting/pluginterfacesupport.cpp" \
  "$B/public.sdk/source/vst/hosting/plugprovider.cpp" \
  "$B/public.sdk/source/vst/hosting/processdata.cpp" \
  -lpthread -ldl -lstdc++ -lm

echo "built: $OUT"
