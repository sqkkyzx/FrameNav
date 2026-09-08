"""Diagnostic partition and production-build isolation."""
import unittest
import configparser
from host_phase1 import ROOT, run_c

class DebugChecks(unittest.TestCase):
    def test_partition_preserves_existing_data(self):
        def rows(name):
            return [s.strip() for s in (ROOT/name).read_text().splitlines()
                    if s.strip() and not s.lstrip().startswith('#')]
        normal=rows('partitions.csv')
        debug=rows('partitions.motion-debug.csv')
        def fields(lines):
            return [[f.strip() for f in s.split(',')] for s in lines]
        self.assertEqual(fields(debug[:len(normal)]),fields(normal))
        self.assertIn('0x377000',debug[-1])
        self.assertIn('0x10000',debug[-1])

    def test_only_debug_target_enables_logging(self):
        cfg=configparser.ConfigParser(interpolation=None)
        cfg.read(ROOT/'platformio.ini')
        enabled=[s for s in cfg.sections() if 'FRAMENAV_TEST_DEBUG=1' in cfg.get(s,'build_flags',fallback='')]
        self.assertEqual(enabled,['env:xiao-c6-motion-debug'])

    def test_disabled_logging_does_not_evaluate_arguments(self):
        header=(ROOT/'include/test_debug.h').read_text().replace('#pragma once','')
        run_c(header+'\n#include <assert.h>\nint main(void){int n=0;test_debug_start();test_debug_event(DBG_BOOT,++n,0,0,0);test_debug_flush();assert(n==0);return 0;}')
