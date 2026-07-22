# Concepts

Shared domain vocabulary for this project — entities, named processes, and status concepts with project-specific meaning. Seeded with core domain vocabulary, then accretes as ce-compound and ce-compound-refresh process learnings; direct edits are fine. Glossary only, not a spec or catch-all.

## Display

### EVE
The Bridgetek "Embedded Video Engine" family of graphics controllers that drives the dash panel over SPI. Generations are numbered (EVE2 through EVE4) and differ in registers and features; the driver library gates its code on the selected generation, so the generation is part of the display's identity, not an implementation detail.

EVE renders each frame from a Display List rather than exposing a framebuffer — the microcontroller never pushes pixels, it describes the scene. Commands reach the chip through a bounded command FIFO, so oversized transfers are a correctness concern, not just a performance one.

### RiBUS
Riverdi's standard host connector for its intelligent displays: one small ZIF ribbon carrying the module's logic power, the SPI/QSPI link, the control signals, and the backlight supply. The module-power pair sits at one end and the backlight pair at the other, with the backlight ground internally tied to logic ground — which makes seating, orientation, and end-identification faults *power* faults, not just signal faults. The internal ground tie doubles as a safe continuity test for identifying the backlight end before applying backlight voltage.

### Display Profile
The compile-time selection that binds the driver library to one specific panel: resolution, sync timings, pixel clock, backlight behavior, and EVE generation all come from the chosen profile, and exactly one profile is enabled per build. Choosing a profile is a claim about the physical panel attached — a wrong profile still initializes and passes chip-identity checks but renders garbage, which is why profiles are verified against the panel's model number and controller rather than by name.

### Display List
The bounded sequence of drawing commands EVE executes to render a frame. The microcontroller builds a new list, then swaps it in atomically; the previous list keeps rendering until the swap, so partial updates are never visible. A list is size-limited, so complex scenes are composed by appending previously built fragments rather than growing a single list without bound.

### Bitmap Handle
One of the EVE chip's small set of per-frame bitmap state slots — each holds one bitmap's source, layout, size, and format, and drawing reads whatever the currently selected handle carries. Fonts claim handles for the whole frame once registered, so anything that configures "the current handle" must first select a scratch handle no font uses and restore the default afterwards — otherwise it silently retargets a font, and that font's glyphs render as garbage while every health signal stays clean.

### RAM_G
EVE's fixed-size on-chip graphics memory — the home of any bitmap the chip decodes or the firmware uploads at runtime, and the fastest asset storage the renderer has. Its capacity is a hard budget that shapes asset decisions: storage formats, downscaling with render-time upscaling, and which assets are resident at once. One caveat: the on-chip PNG decoder borrows the top of RAM_G as scratch during image loads, so anything packed near the top must leave it headroom. Rendering straight from the panel's own flash once served as an escape valve for small assets, but its per-frame ceiling (see Flash Render Streaming) meant large assets needed RAM_G staging anyway, and the flash path has been retired.

### Asset Pack
The versioned bundle of ASTC-compressed static assets embedded in the firmware image and staged directly into RAM_G at boot, with each asset verified against the embedded source before the renderer may use it — a failed check skips that asset for the session. The pack formerly lived provisioned in the display module's onboard flash; that round trip was retired because the pack ships in firmware either way, staging rides the same proven command path as font loading, and boot no longer depends on the module's flash at all. Staging time is a function of the SPI Operating Point.

### Flash Render Streaming
Rendering a bitmap by having the display's graphics engine fetch it from the module's onboard flash every frame, rather than from RAM_G. It was the mechanism that let flash-resident assets cost no graphics memory, and it has a per-frame throughput ceiling: past a size threshold an asset arrives too slowly to complete the frame, and renders with correct content broken by dropped scanlines. The ceiling is why the firmware retired flash-resident rendering in favor of staging everything into RAM_G.

The ceiling is a property of the path, not a fault in a given display module, and reaching the flash controller's full-speed mode does not lift it. The distinguishing symptom is that content is *correct but torn* — genuinely corrupt data instead points at addressing, alignment, or block-ordering faults in the pack. Assets above the threshold must be staged into RAM_G at boot; below it, streaming is free.

### Splash Theme
One of several complete visual variants of the boot splash — background, accent hardware, and year mark as a matched set. Every theme ships embedded in the firmware; which one plays is a build-time selection, since the panel has no input hardware for runtime switching. Changing themes means rebuilding and reflashing, never editing assets or code.

### Dash Mode
The dash's active view: TRACK (shift lights, speed hero, lap timing) or STREET (sweep gauges, telltales, odometer). All screens switch together and instantly, with no state loss. The selection is an external input to the firmware — a serial command during bench development, a CAN message in the car — never something the dash decides for itself.

### Alarm Takeover
A full-screen flashing overlay that preempts the active Dash Mode on the center screen while any critical engine condition holds, showing only the highest-priority active alarm with its live value and limit. The side panels are deliberately not preempted — the Engine Screen keeps showing live vitals during the event. It clears itself when the condition clears; a missing data channel never triggers or sustains one.

### Engine Screen
The left 5" side panel: engine vitals sourced from engine-side CAN (oil pressure/temp, coolant, fuel pressure, AFR, IAT, volts). Renders a dense data grid in TRACK and mini sweep gauges in STREET, and stays live during an Alarm Takeover.

### Timing Screen
The right 5" side panel: TIMING in TRACK mode (lap number, position, last/best/predicted times, throttle and brake bars) and ROAD in STREET mode (fuel gauge, trip, range, ambient, clock). Sourced from RaceCapture-side data once CAN lands.

### SPI Operating Point
The bus clock the dash runs at after every panel has initialized — distinct from the slower init clock the display controller's datasheet mandates during bring-up, which is why chip identity can read healthy while the operating point is still unproven. The raise happens once, bus-wide, and the value is owned by bench evidence from the actual wiring, not the chip's rated ceiling. When the physical link changes — bench loom to Carrier Board — the prior operating point becomes historical evidence, and a Clock Walk re-owns the value on the new wiring.

An operating point is accepted only by a read-integrity soak — repeated register reads with zero anomalies — never by frame rate alone: bus corruption can garble rendering and sag the frame rate while every automatic fault counter stays at zero, because the fault detectors check what the chip reports, not whether the read itself was clean.

### Clock Walk
The stepwise process of raising the SPI Operating Point: one clock step at a time, each step accepted or rejected solely by the read-integrity soak before the next is attempted. The walk's diagnosis rules depend on the link: reads failing while writes stay clean points at round-trip latency — answered by delaying when the return data is sampled — whereas writes failing too means genuine signal degradation and a retreat. Frame rate is never an acceptance signal at any step.

### Carrier Board
The purpose-built PCB that replaces the bench wiring loom as the dash's physical platform: it hosts the microcontroller, gives each panel its own buffered, terminated point-to-point SPI leg with a dedicated RiBUS connector, gates each panel's read-return line by that panel's own chip select, and carries the power regulation for both logic rails. Its existence splits the project's electrical history in two — measurements and operating points established on the loom describe the loom, not the system.

### Data Channel
One live value the dash consumes (RPM, oil pressure, lap delta…), carried in a single shared structure with a per-channel validity flag. Producers fill channels — the built-in simulator today, CAN decoders later — and renderers only read them; the source is invisible to rendering. An invalid channel displays `--` and can never assert an alarm, which is what makes "no stale alarms" a structural guarantee rather than a convention.
