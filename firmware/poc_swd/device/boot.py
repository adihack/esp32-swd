# SPDX-License-Identifier: MIT
# boot.py - runs first on every boot and soft reset: every isolation switch open, every driver idle.
import r5
r5.safe_all()
