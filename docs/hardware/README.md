# Hardware design documents (copies from the KiCad project, 2026-11-06)

These are copied as they stood when board r5 was built. They describe how the hardware got to its current form.

- `design-brief.md`: the original requirements, still valid for the firmware: host protocol, states, sequence,
  leakage targets.
- `architecture.md`: decisions and revision history.
  - Its sections "Key part choices", "Signal chain", "Power tree" and "Known risks" still describe the first,
    1.6–3.6 V design. That design had translators, VIO regulators, an I/O expander, LEDs and buttons, none of which
    are on r5.
  - The revision notes at the top override those sections.
- `pcb-layout.md`: the layout and routing process.

For firmware, use `../hardware-reference.md`. It was built from the r5 netlist and wins wherever these files disagree.
