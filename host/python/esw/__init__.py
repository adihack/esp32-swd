# SPDX-License-Identifier: MIT
"""esp-swd host library."""
from .client import Esw, EswError  # noqa: F401
from . import proto  # noqa: F401

CH_A, CH_B = 0, 1
