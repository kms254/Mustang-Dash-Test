# Concepts

Shared domain vocabulary for this project — entities, named processes, and status concepts with project-specific meaning. Seeded with core domain vocabulary, then accretes as ce-compound and ce-compound-refresh process learnings; direct edits are fine. Glossary only, not a spec or catch-all.

## Display

### EVE
The Bridgetek "Embedded Video Engine" family of graphics controllers that drives the dash panel over SPI. Generations are numbered (EVE2 through EVE4) and differ in registers and features; the driver library gates its code on the selected generation, so the generation is part of the display's identity, not an implementation detail.

EVE renders each frame from a Display List rather than exposing a framebuffer — the microcontroller never pushes pixels, it describes the scene. Commands reach the chip through a bounded command FIFO, so oversized transfers are a correctness concern, not just a performance one.

### Display Profile
The compile-time selection that binds the driver library to one specific panel: resolution, sync timings, pixel clock, backlight behavior, and EVE generation all come from the chosen profile, and exactly one profile is enabled per build. Choosing a profile is a claim about the physical panel attached — a wrong profile still initializes and passes chip-identity checks but renders garbage, which is why profiles are verified against the panel's model number and controller rather than by name.

### Display List
The bounded sequence of drawing commands EVE executes to render a frame. The microcontroller builds a new list, then swaps it in atomically; the previous list keeps rendering until the swap, so partial updates are never visible. A list is size-limited, so complex scenes are composed by appending previously built fragments rather than growing a single list without bound.
