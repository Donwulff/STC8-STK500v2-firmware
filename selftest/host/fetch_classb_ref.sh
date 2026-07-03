#!/bin/sh
# Fetch the Microchip IEC 60730 Class B Diagnostic Library sources for
# REFERENCE STUDY.  They are published on npm (MCC Melody module) with the
# full C implementation of every diagnostic under
#   package/output/submodules/c_library/avr8-diag-*/
#
# NOT vendored into this repo: the Microchip license permits use with
# Microchip products but is not an OSI license, so our tests are original
# implementations of the public-literature algorithms (March C-: van de
# Goor; checkerboard register/SREG/SP walk; CRC16).  Keep it that way if
# you touch the test code.  ref/ is expected to be gitignored.
set -e
cd "$(dirname "$0")/.."
mkdir -p ref
cd ref
npm pack @mchp-mcc/avr-iec60730-class-b-diagnostic-library
tar xzf mchp-mcc-avr-iec60730-class-b-diagnostic-library-*.tgz
echo
echo "Diagnostic sources: ref/package/output/submodules/c_library/"
echo "License: Microchip 'use with Microchip products' terms — reference only."
