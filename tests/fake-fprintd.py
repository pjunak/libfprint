#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Scripted fprintd and logind for tests/test-pam-parallel.py.

Usage: fake-fprintd.py BUS_ADDRESS SCENARIO_JSON LOG_FILE

Scenario keys:
  enrolled      list of enrolled fingers (default ["right-index-finger"])
  busy_claims   how many Claim calls fail with AlreadyInUse first (default 0)
  verify        one entry per VerifyStart: "match", "no-match", "wait", or a
                list of results ending in one of those, e.g. ["too-short", "match"]
  delay_ms      delay before each result (default 200)
  locks         [[ms, true|false], ...] LockedHint changes of session "test"

The protocol is enforced like fprintd: a second VerifyStart needs VerifyStop
first, Release needs a claim. Every call is appended to LOG_FILE.
"""
import json
import sys

import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib  # noqa: E402

FPRINT_XML = '''<node>
  <interface name="net.reactivated.Fprint.Manager">
    <method name="GetDefaultDevice"><arg type="o" direction="out"/></method>
  </interface>
  <interface name="net.reactivated.Fprint.Device">
    <method name="ListEnrolledFingers"><arg type="s" direction="in"/><arg type="as" direction="out"/></method>
    <method name="Claim"><arg type="s" direction="in"/></method>
    <method name="Release"/>
    <method name="VerifyStart"><arg type="s" direction="in"/></method>
    <method name="VerifyStop"/>
    <signal name="VerifyStatus"><arg type="s"/><arg type="b"/></signal>
    <property name="scan-type" type="s" access="read"/>
  </interface>
</node>'''
LOGIN_XML = '''<node>
  <interface name="org.freedesktop.login1.Manager">
    <method name="GetSession"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
  </interface>
  <interface name="org.freedesktop.login1.Session">
    <property name="LockedHint" type="b" access="read"/>
  </interface>
</node>'''
DEVICE = '/net/reactivated/Fprint/Device/0'
SESSION = '/org/freedesktop/login1/session/test'
RESULTS = {'match': ('verify-match', True), 'no-match': ('verify-no-match', True),
           'too-short': ('verify-swipe-too-short', False), 'retry': ('verify-retry-scan', False),
           'unknown-error': ('verify-unknown-error', True)}


class Fake:
    def __init__(self, connection, scenario, log):
        self.connection, self.scenario, self.log = connection, scenario, log
        self.claimed = self.verifying = False
        self.busy = scenario.get('busy_claims', 0)
        self.verify = list(scenario.get('verify', ['wait']))
        self.locked = False
        self.timer = 0

    def record(self, text):
        with open(self.log, 'a') as out:
            out.write(text + '\n')

    def emit(self, results):
        self.timer = 0
        name, done = RESULTS[results[0]]
        self.connection.emit_signal(None, DEVICE, 'net.reactivated.Fprint.Device', 'VerifyStatus',
                                    GLib.Variant('(sb)', (name, done)))
        self.record(f'emit {name}')
        if len(results) > 1 and not done:
            self.schedule(results[1:])
        return GLib.SOURCE_REMOVE

    def schedule(self, results):
        if results and results[0] != 'wait':
            self.timer = GLib.timeout_add(self.scenario.get('delay_ms', 200), self.emit, results)

    def fprint_call(self, connection, sender, path, iface, method, params, invocation):
        args = params.unpack()
        self.record(' '.join([method, *map(str, args)]))
        error = None
        if method == 'GetDefaultDevice':
            return invocation.return_value(GLib.Variant('(o)', (DEVICE,)))
        if method == 'ListEnrolledFingers':
            fingers = self.scenario.get('enrolled', ['right-index-finger'])
            if not fingers:
                error = ('NoEnrolledPrints', 'no prints')
            else:
                return invocation.return_value(GLib.Variant('(as)', (fingers,)))
        elif method == 'Claim':
            if self.busy:
                self.busy -= 1
                error = ('AlreadyInUse', 'claimed by another client')
            elif self.claimed:
                error = ('AlreadyInUse', 'already claimed')
            else:
                self.claimed = True
        elif method == 'Release':
            if not self.claimed or self.verifying:
                error = ('ClaimDevice', 'not claimed or still verifying')
            self.claimed = False
        elif method == 'VerifyStart':
            if not self.claimed or self.verifying:
                error = ('AlreadyInUse', 'verification already running')
            else:
                self.verifying = True
                entry = self.verify.pop(0) if self.verify else 'wait'
                self.schedule(entry if isinstance(entry, list) else [entry])
        elif method == 'VerifyStop':
            if not self.verifying:
                error = ('NoActionInProgress', 'no verification')
            self.verifying = False
            if self.timer:
                GLib.source_remove(self.timer)
                self.timer = 0
        if error:
            self.record(f'error {error[0]}')
            return invocation.return_dbus_error('net.reactivated.Fprint.Error.' + error[0], error[1])
        invocation.return_value(None)

    def fprint_property(self, connection, sender, path, iface, name):
        return GLib.Variant('s', 'swipe')

    def login_call(self, connection, sender, path, iface, method, params, invocation):
        self.record(f'GetSession {params.unpack()[0]}')
        invocation.return_value(GLib.Variant('(o)', (SESSION,)))

    def login_property(self, connection, sender, path, iface, name):
        return GLib.Variant('b', self.locked)

    def set_locked(self, locked):
        self.locked = locked
        self.record(f'locked {locked}')
        self.connection.emit_signal(None, SESSION, 'org.freedesktop.DBus.Properties', 'PropertiesChanged',
                                    GLib.Variant('(sa{sv}as)', ('org.freedesktop.login1.Session',
                                                                {'LockedHint': GLib.Variant('b', locked)}, [])))
        return GLib.SOURCE_REMOVE


def main():
    address, scenario, log = sys.argv[1], json.loads(sys.argv[2]), sys.argv[3]
    flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
    connection = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
    fake = Fake(connection, scenario, log)
    fprint = Gio.DBusNodeInfo.new_for_xml(FPRINT_XML)
    login = Gio.DBusNodeInfo.new_for_xml(LOGIN_XML)
    connection.register_object('/net/reactivated/Fprint/Manager', fprint.interfaces[0], fake.fprint_call)
    connection.register_object(DEVICE, fprint.interfaces[1], fake.fprint_call, fake.fprint_property)
    connection.register_object('/org/freedesktop/login1', login.interfaces[0], fake.login_call)
    connection.register_object(SESSION, login.interfaces[1], None, fake.login_property)
    for name in ('net.reactivated.Fprint', 'org.freedesktop.login1'):
        connection.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                             'RequestName', GLib.Variant('(su)', (name, 4)), None, 0, -1, None)
    for delay, locked in scenario.get('locks', []):
        GLib.timeout_add(delay, fake.set_locked, locked)
    print('READY', flush=True)
    GLib.MainLoop().run()


if __name__ == '__main__':
    main()
