# The source lists, shared by build.sh and test/run_tests.sh.
# the plugin (C++17)
ENGINE_SRCS="src/engine.cc src/source.cc src/net.cc"
# Dragonfly Reverb's Plate and Room (Michael Willis, GPL-3.0; freeverb3 inside), as RMXXXL builds them (C++11)
FV=third_party/dragonfly/common/freeverb
DF_SRCS="src/rv_plate.cc src/rv_room.cc $FV/allpass.cpp $FV/biquad.cpp $FV/comb.cpp $FV/delay.cpp $FV/delayline.cpp \
 $FV/earlyref.cpp $FV/efilter.cpp $FV/nrev.cpp $FV/nrevb.cpp $FV/progenitor.cpp $FV/progenitor2.cpp $FV/revbase.cpp \
 $FV/slot.cpp $FV/strev.cpp $FV/utils.cpp $FV/zrev.cpp $FV/zrev2.cpp"
# Mutable Instruments Clouds with the Parasites firmware (MIT), as RMXXXL vendors it
PA=third_party/parasites
CL_SRCS="$PA/clouds/dsp/correlator.cc $PA/clouds/dsp/granular_processor.cc $PA/clouds/dsp/mu_law.cc \
 $PA/clouds/dsp/pvoc/frame_transformation.cc $PA/clouds/dsp/pvoc/phase_vocoder.cc $PA/clouds/dsp/pvoc/stft.cc \
 $PA/clouds/resources.cc $PA/stmlib/dsp/atan.cc $PA/stmlib/dsp/units.cc $PA/stmlib/utils/random.cc"
INCS="-Isrc -Ithird_party/parasites -Isrc/juce_shim -Ithird_party/curl/include -Ithird_party/dragonfly/common -Ithird_party/dragonfly/shim"
DEFS="-D_FILE_OFFSET_BITS=64 -DLIBFV3_FLOAT -DTEST"
