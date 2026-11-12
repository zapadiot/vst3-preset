#!/bin/bash
set -e
SRC=/home/baboon/vstaudio_host.cpp
B=/home/baboon/vst3sdk
FLAGS="-I$B -Wno-multichar -O2 -DNDEBUG -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -DSMTG_RENAME_ASSERT=1 -DRELEASE=1"
/usr/bin/c++ $FLAGS -o /home/baboon/vstaudio_host \
  "$SRC" \
  $B/base/source/baseiids.cpp \
  $B/base/source/fbuffer.cpp \
  $B/base/source/fdebug.cpp \
  $B/base/source/fdynlib.cpp \
  $B/base/source/fobject.cpp \
  $B/base/source/fstreamer.cpp \
  $B/base/source/fstring.cpp \
  $B/base/source/timer.cpp \
  $B/base/source/updatehandler.cpp \
  $B/base/thread/source/fcondition.cpp \
  $B/base/thread/source/flock.cpp \
  $B/pluginterfaces/base/conststringtable.cpp \
  $B/pluginterfaces/base/coreiids.cpp \
  $B/pluginterfaces/base/funknown.cpp \
  $B/pluginterfaces/base/ustring.cpp \
  $B/public.sdk/source/common/commoniids.cpp \
  $B/public.sdk/source/common/commonstringconvert.cpp \
  $B/public.sdk/source/common/memorystream.cpp \
  $B/public.sdk/source/common/openurl.cpp \
  $B/public.sdk/source/common/pluginview.cpp \
  $B/public.sdk/source/common/readfile.cpp \
  $B/public.sdk/source/common/systemclipboard_linux.cpp \
  $B/public.sdk/source/common/threadchecker_linux.cpp \
  $B/public.sdk/source/main/moduleinit.cpp \
  $B/public.sdk/source/main/linuxmain.cpp \
  $B/public.sdk/source/main/pluginfactory.cpp \
  $B/public.sdk/source/vst/vstaudioeffect.cpp \
  $B/public.sdk/source/vst/vstbus.cpp \
  $B/public.sdk/source/vst/vstcomponent.cpp \
  $B/public.sdk/source/vst/vstcomponentbase.cpp \
  $B/public.sdk/source/vst/vsteditcontroller.cpp \
  $B/public.sdk/source/vst/vstinitiids.cpp \
  $B/public.sdk/source/vst/vstnoteexpressiontypes.cpp \
  $B/public.sdk/source/vst/vstparameters.cpp \
  $B/public.sdk/source/vst/vstrepresentation.cpp \
  $B/public.sdk/source/vst/utility/dataexchange.cpp \
  $B/public.sdk/source/vst/utility/stringconvert.cpp \
  $B/public.sdk/source/vst/utility/systemtime.cpp \
  $B/public.sdk/source/vst/hosting/connectionproxy.cpp \
  $B/public.sdk/source/vst/hosting/eventlist.cpp \
  $B/public.sdk/source/vst/hosting/hostclasses.cpp \
  $B/public.sdk/source/vst/hosting/hostdataexchangehandler.cpp \
  $B/public.sdk/source/vst/hosting/module.cpp \
  $B/public.sdk/source/vst/hosting/module_linux.cpp \
  $B/public.sdk/source/vst/hosting/parameterchanges.cpp \
  $B/public.sdk/source/vst/hosting/pluginterfacesupport.cpp \
  $B/public.sdk/source/vst/hosting/plugprovider.cpp \
  $B/public.sdk/source/vst/hosting/processdata.cpp \
  -lpthread -ldl -lstdc++ -lm -lasound
