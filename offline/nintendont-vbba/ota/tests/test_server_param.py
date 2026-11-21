#!/usr/bin/env python3
# Copyright (C) 2026 Nigzel / Bjerreman. SPDX-License-Identifier: GPL-3.0-or-later OR GPL-2.0-only
"""The update server of the shell is a build parameter (ota/stage_loader.py server_values): environment first, then a profile
file, else the documentation placeholder; a release or shell build refuses the placeholder.  Run: python3 test_server_param.py"""
import json, os, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import stage_loader as S  # noqa: E402

KEYS = ("MS_SERVER_IP", "MS_SERVER_HOST", "MS_SERVER_PROFILE", "NIN_MS_QUIET", "NIN_MS_SHELL_SEQ", "MS_SERVER_PLACEHOLDER_OK")


class ServerParameter(unittest.TestCase):
    def setUp(self):
        self.saved = {k: os.environ.pop(k) for k in KEYS if k in os.environ}
        self.repo = tempfile.mkdtemp()                      # a tree without a private profile

    def tearDown(self):
        for k in KEYS:
            os.environ.pop(k, None)
        os.environ.update(self.saved)

    def test_environment_wins(self):
        os.environ.update(MS_SERVER_IP="203.0.113.7", MS_SERVER_HOST="updates.example.org")
        self.assertEqual(S.server_values(self.repo), ("203.0.113.7", "updates.example.org", "environment"))

    def test_profile_file(self):
        p = os.path.join(self.repo, "profile.json")
        json.dump({"online": {"server_ip": "198.51.100.9", "ota_http_host": "srv.example.net"}}, open(p, "w"))
        os.environ["MS_SERVER_PROFILE"] = p
        self.assertEqual(S.server_values(self.repo), ("198.51.100.9", "srv.example.net", "private server profile"))

    def test_placeholder_for_a_dev_build(self):
        ip, host, how = S.server_values(self.repo)
        self.assertEqual((ip, host, how), (S.PLACEHOLDER_IP, S.PLACEHOLDER_HOST, "placeholder"))

    def test_release_and_shell_builds_refuse_the_placeholder(self):
        for k, v in (("NIN_MS_QUIET", "1"), ("NIN_MS_SHELL_SEQ", "0")):
            os.environ[k] = v
            with self.assertRaises(SystemExit):
                S.server_values(self.repo)
            os.environ["MS_SERVER_PLACEHOLDER_OK"] = "1"
            self.assertEqual(S.server_values(self.repo)[2], "placeholder")
            del os.environ[k], os.environ["MS_SERVER_PLACEHOLDER_OK"]

    def test_bad_values(self):
        os.environ.update(MS_SERVER_IP="300.1.1.1", MS_SERVER_HOST="x.example")
        with self.assertRaises(SystemExit):
            S.server_values(self.repo)
        os.environ.update(MS_SERVER_IP="192.0.2.3", MS_SERVER_HOST="bad host")
        with self.assertRaises(SystemExit):
            S.server_values(self.repo)


if __name__ == "__main__":
    unittest.main()
