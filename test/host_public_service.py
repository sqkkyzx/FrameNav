#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Exercise the public page's browser-facing location and BLE flow."""

import shutil
import subprocess
import unittest

from host_phase1 import ROOT


class PublicServiceChecks(unittest.TestCase):
    @unittest.skipUnless(shutil.which("node"), "requires Node.js")
    def test_browser_connect_location_write_clear_and_secure_context(self):
        app = (ROOT / "public-service/site/app.js").read_text()
        self.assertEqual(subprocess.run(
            [shutil.which("node"), "--check", str(ROOT / "public-service/site/app.js")],
            capture_output=True, text=True,
        ).returncode, 0)
        harness = r"""
const vm = require('node:vm'), assert = require('node:assert/strict');
const SERVICE = 'b1f0b4d5-797b-5a9e-5b4f-4a1f02007ea1';
const WRITE = 'b1f0b4d5-797b-5a9e-5b4f-4a1f02107ea1';
const CLEAR = 'b1f0b4d5-797b-5a9e-5b4f-4a1f02117ea1';
const nodes = new Map(), writes = [], geo = [];
function node(id) {
  if (!nodes.has(id)) nodes.set(id, {id, value:'', textContent:'', className:'', disabled:false,
    events:{}, classList:{toggle(){}}, addEventListener(type, callback){this.events[type]=callback;}});
  return nodes.get(id);
}
const writeChar = {async writeValueWithResponse(value){writes.push([...new Uint8Array(value)]);}};
const clearChar = {async writeValueWithResponse(value){writes.push([...new Uint8Array(value)]);}};
const service = {async getCharacteristic(uuid){assert([WRITE,CLEAR].includes(uuid)); return uuid===WRITE ? writeChar : clearChar;}};
const device = {name:'FrameNav-ABC123', addEventListener(){}, gatt:{connected:true,
  async connect(){return {async getPrimaryService(uuid){assert.equal(uuid,SERVICE);return service;}};}}};
const navigator = {bluetooth:{async requestDevice(options){
  assert.equal(options.filters.length, 1);
  assert.equal(options.filters[0].services.length, 1);
  assert.equal(options.filters[0].services[0], SERVICE);
  return device;
}},
  geolocation:{getCurrentPosition(ok,fail,options){geo.push(options);if(options.enableHighAccuracy)
    ok({coords:{latitude:23.5,longitude:114.5,accuracy:16.7},timestamp:Date.now()});else fail({code:3});}}};
const context = {window:{isSecureContext:true},navigator,document:{getElementById:node},Date,Number,Math,BigInt,
  DataView,ArrayBuffer,Uint8Array,Promise};
vm.createContext(context);
""" + "vm.runInContext(" + __import__("json").dumps(app) + r""", context);
(async()=>{
  await node('connect').events.click();
  assert.equal(node('device-state').textContent, '已连接');
  await node('get-location').events.click();
  assert.equal(geo.length, 2);
  assert.equal(geo[0].maximumAge, 0);
  assert.equal(geo[1].enableHighAccuracy, true);
  assert.equal(node('write-location').disabled, false);
  await node('write-location').events.click();
  assert.equal(writes.length, 1);
  assert.equal(writes[0].length, 28);
  const view = new DataView(new Uint8Array(writes[0]).buffer);
  assert.equal(view.getInt32(0, true), 235000000);
  assert.equal(view.getInt32(4, true), 1145000000);
  assert.equal(view.getUint32(8, true), 1670);
  node('manual-latitude').value='24.1'; node('manual-longitude').value='115.2';
  await node('write-manual').events.click();
  assert.equal(writes.length, 2);
  assert.equal(new DataView(new Uint8Array(writes[1]).buffer).getInt32(0, true), 241000000);
  await node('clear-location').events.click();
  assert.deepEqual(writes[2], [1]);
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
        result = subprocess.run(
            [shutil.which("node"), "-e", harness], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
