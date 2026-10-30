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
const nodes = new Map(), writes = [], geo = [], geoErrors = [], geoPositions = [], writeErrors = [];
let activeWrites = 0, maxActiveWrites = 0;
function node(id) {
  if (!nodes.has(id)) nodes.set(id, {id, value:'', textContent:'', className:'', disabled:false,
    events:{}, classList:{toggle(){}}, addEventListener(type, callback){this.events[type]=callback;}});
  return nodes.get(id);
}
const writeChar = {async writeValueWithResponse(value){
  activeWrites += 1; maxActiveWrites = Math.max(maxActiveWrites, activeWrites);
  await Promise.resolve();
  if (writeErrors.length) { const error = writeErrors.shift(); activeWrites -= 1; throw error; }
  writes.push([...new Uint8Array(value)]); activeWrites -= 1;
}};
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
  geolocation:{getCurrentPosition(ok,fail,options){geo.push({ok,fail,options});
    if(geoErrors.length){fail(geoErrors.shift());return;}
    if(geoPositions.length){ok(geoPositions.shift());return;}
    if(options.enableHighAccuracy) ok({coords:{latitude:23.5,longitude:114.5,accuracy:16.7},timestamp:Date.now()});
    else fail({code:3});}}};
const context = {window:{isSecureContext:true},navigator,document:{getElementById:node},Date,Number,Math,BigInt,setTimeout,
  DataView,ArrayBuffer,Uint8Array,Promise};
vm.createContext(context);
""" + "vm.runInContext(" + __import__("json").dumps(app) + r""", context);
(async()=>{
  await node('connect').events.click();
  assert.equal(node('device-state').textContent, '已连接');
  await node('get-location').events.click();
  assert.equal(geo.length, 2);
  assert.equal(geo[0].options.maximumAge, 60000);
  assert.equal(geo[1].options.maximumAge, 60000);
  assert.equal(geo[1].options.enableHighAccuracy, true);
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
  writeErrors.push(new Error('GATT operation already in progress.'),
                   new Error('GATT operation failed for unknown reason.'));
  const firstWrite = node('write-location').events.click();
  const secondWrite = node('write-location').events.click();
  await Promise.all([firstWrite, secondWrite]);
  assert.equal(maxActiveWrites, 1);
  assert.equal(writes.length, 5);
  assert.match(node('message').textContent, /已写入设备/);
  geoErrors.push({code:1});
  await node('get-location').events.click();
  assert.match(node('message').textContent, /Android 设置中允许 Chrome/);
  geoErrors.push({code:2}, {code:2});
  await node('get-location').events.click();
  assert.equal(geo.length, 5);
  assert.match(node('message').textContent, /定位服务暂不可用（错误 2）/);
  geoErrors.push({code:3}, {code:3});
  await node('get-location').events.click();
  assert.equal(geo.length, 7);
  assert.match(node('message').textContent, /定位超时（错误 3）/);
  geoPositions.push({coords:{latitude:31.2,longitude:121.4,accuracy:35},timestamp:Date.now()});
  await node('get-location').events.click();
  assert.equal(geo.length, 8);
  assert.equal(node('latitude').textContent, '31.2000000');
  assert.equal(node('write-location').disabled, false);
  context.navigator = {};
  await node('connect').events.click();
  assert.match(node('message').textContent, /未提供 Web Bluetooth/);
})().catch(error=>{console.error(error);process.exitCode=1;});
"""
        result = subprocess.run(
            [shutil.which("node"), "-e", harness], capture_output=True, text=True
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("请使用 Android Chrome", app)
        self.assertNotIn("需 Android Chrome", app)


if __name__ == "__main__":
    unittest.main(verbosity=2)
