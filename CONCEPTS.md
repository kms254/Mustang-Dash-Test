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

### Font Instance
One typeface rendered at one pixel size into a fixed-cell glyph sheet, claiming its own Bitmap Handle and uploaded to RAM_G at boot. Each instance carries its own character set, and that set is stored as a contiguous run of character codes rather than a sparse map — so an instance's memory cost is set by the span between its lowest and highest character, not by how many characters it actually uses.

Every cell within an instance is sized to fit its largest glyph, so introducing one wide character enlarges every other cell as well, including ones already present. Both properties make "add one character" a change worth costing before making rather than after.

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

### Laps Left
How many more laps the remaining fuel will actually complete — a range estimate, not a count of laps driven. Deliberately excludes an unusable reserve at the bottom of the tank, because sustained cornering starves the pickup well before the tank is dry, so laps promised out of that last fuel are laps the car will not finish.

It therefore reaches zero while the fuel gauge still shows fuel, and that divergence is the concept working rather than a defect. It is also the one figure permitted to disagree with the simulator's own tank, which runs to empty because it models fuel quantity and not pickup behavior; the per-lap burn *rate* behind the estimate must still match what the car actually consumes. Being one word away from a live lap number on the same screen, it is easy to misread as a lap counter — its label has to carry the distinction.

### SPI Operating Point
The bus clock the dash runs at after every panel has initialized — distinct from the slower init clock the display controller's datasheet mandates during bring-up, which is why chip identity can read healthy while the operating point is still unproven. The raise happens once, bus-wide, and the value is owned by bench evidence from the actual wiring, not the chip's rated ceiling. When the physical link changes — bench loom to Carrier Board — the prior operating point becomes historical evidence, and a Clock Walk re-owns the value on the new wiring.

An operating point is accepted only by a read-integrity soak — repeated register reads with zero anomalies — never by frame rate alone: bus corruption can garble rendering and sag the frame rate while every automatic fault counter stays at zero, because the fault detectors check what the chip reports, not whether the read itself was clean.

### Clock Walk
The stepwise process of raising the SPI Operating Point: one clock step at a time, each step accepted or rejected solely by the read-integrity soak before the next is attempted. The walk's diagnosis rules depend on the link: reads failing while writes stay clean points at round-trip latency — answered by delaying when the return data is sampled — whereas writes failing too means genuine signal degradation and a retreat. A step can also fail past degradation into a total wedge: reads corrupt enough that the command-wait loop never exits, the firmware freezes, and the failure presents as sudden silence rather than glitches — recovery is a debug-probe reflash, not a power cycle. Frame rate is never an acceptance signal at any step, and every accepted step also requires eyes on the panel.

### Frame Drain
The per-frame wait for the display controller to finish executing a display list before its command buffer is reused — the renderer polls the chip's command-FIFO registers over SPI until the coprocessor signals completion. Because that poll is a read, it is the frame loop's most exposed point to a marginal link: a bounded drain that never sees completion times out rather than hanging forever, and a timed-out drain retires the panel (see Retired Panel). A drain timeout therefore indicts the read path — clock, wiring, or the shared ground reference — not the rendering itself.

### Retired Panel
A panel the firmware has marked dead at runtime and now skips, after its Frame Drain timed out — distinct from a panel that merely failed to initialize at boot. A retired panel shows no image and reports its liveness field as absent while the healthy panels keep rendering, so one panel's read-path failure never blocks the others. Retirement is a runtime verdict on the link, not the silicon: the same panel typically returns clean once the read path (grounding, wiring, clock) is sound.

### Carrier Board
The purpose-built PCB that replaces the bench wiring loom as the dash's physical platform: it hosts the microcontroller, gives each panel its own buffered, terminated point-to-point SPI leg with a dedicated RiBUS connector, gates each panel's read-return line by that panel's own chip select, and carries the power regulation for both logic rails. Its existence splits the project's electrical history in two — measurements and operating points established on the loom describe the loom, not the system.

### Data Channel
One live value the dash consumes (RPM, oil pressure, lap delta…), carried in a single shared structure with a per-channel validity flag. Producers fill channels — the built-in simulator today, CAN decoders later — and renderers only read them; the source is invisible to rendering. An invalid channel displays `--` and can never assert an alarm, which is what makes "no stale alarms" a structural guarantee rather than a convention.

## Board Design

### Airwire
One unrouted pad-to-pad connection on a PCB — the thin line a layout tool draws between two pads the netlist says belong together and that no copper yet joins. A single net produces as many airwires as it has connections still to make, so airwires and nets count entirely different things and are not interchangeable.

The distinction is load-bearing here because this project has already confused them once: a routing task recorded as "~274 unrouted nets" was in fact ~274 airwires across roughly a hundred nets, 93 of which already carried copper. Scoped against nets the work looked like most of the board; scoped correctly it was a nearly-complete board. Any statement about how much routing remains is ambiguous unless it names which unit it counts.

### Design Rules
The manufacturing constraints a board is drawn to — minimum clearance, track width, via diameter and drill, hole spacing — held as project metadata alongside the board rather than inside its geometry. Because they live outside the design objects, they travel separately: a tool can carry every footprint, net, and trace faithfully and still lose the rules entirely.

That separation is why rules are verified rather than assumed after any import or handoff. Measured against the wrong rules a correct board reports hundreds of violations, and a router reading those rules will produce copper the fab never agreed to — with nothing in either output indicating the constraints themselves are fiction. The tell is a violation set clustered just under a round number: real sloppiness scatters, a mismatched rule produces a band.

## Track Simulation

### Circuit
The driving model TRACK mode runs behind the screen: the real racetrack lap, or a range-sweep bench fixture whose only job is walking every gauge through its full display range. Distinct from Dash Mode, which chooses *which* screen is shown — Circuit chooses what the simulator is doing behind it. Selecting a circuit abandons the lap in progress rather than resuming it, because a half-driven lap would commit a fabricated time to the lap book.

### Segment
One entry in the distance-keyed table that defines a lap: a length, a speed limit, whether that limit is a real corner constraint or merely a descriptive annotation, and how far the car's heading swings through it. Lap position is a distance along this table rather than a fraction of elapsed time, which is what makes lap time an output of the simulation instead of an input to it. A limit binds at its segment's entry boundary and across the Corner Arc beyond it; past the arc the car accelerates freely until it must brake for the next limit.

### Corner Arc
The stretch of a Segment over which the car is lateral-grip-limited: it holds roughly its corner speed to the apex, then releases progressively as the driver unwinds the wheel. Derived from the segment's *authored* limit and the car's lateral grip — never from the speed the driver actually carries.

That distinction is load-bearing rather than stylistic. A corner is a fixed length of road, so deriving its geometry from live speed makes a slower driver drive a physically shorter corner, which gives back most of the time the lower speed cost and silently cancels Driver Skill's effect on lap time.

An arc **ends by release, not by expiry**, and the difference is visible on the glass. Past the apex, lateral demand tapers toward zero: the speed the corner permits rises as the effective radius opens, and the grip freed up becomes available for acceleration, so by the arc's end the corner has become a straight and crossing that boundary is a non-event. An arc that simply stopped would hand the car its whole lateral budget between two simulation steps — which reads as a throttle slamming open, and in the real car is a spin. Where the apex falls within the arc is calibrated against lap time, because releasing earlier hands the car a share of every corner on the lap.

### Driver Skill
A single scalar standing in for how close to the car's limit the driver operates, scaling corner limits and widening lap-to-lap variation. Segments whose limit is an annotation rather than a real constraint are exempt from it, and it must never reach Corner Arc geometry.

It is calibrated last and held to a defensible range, because it is the only constant tuned directly against lap time and will otherwise absorb error from every upstream constant — at which point it stops meaning "driver" and starts meaning "whatever makes the number come out." A fit that lands outside the defensible range is evidence that an upstream constant is wrong, not a value to accept.

### Roll-on
The bounded rate at which the modelled driver opens the throttle, and the reason a Corner Arc exit is a squeeze rather than a stab: the pedal is a state that may only rise so fast, and it closes fully whenever the car is braking, so every exit builds from nothing.

The pedal is the *input* to the acceleration model, not a smoothing of its output — thrust is computed from the pedal, so the bar on the screen and the car's behavior cannot disagree. Modelling it the other way, as a filter on the displayed value, would show a gentle bar while the car still took its full acceleration instantly. It also costs real lap time, which is the honest consequence of a driver who squeezes: a model without it laps quicker than the car can.

A Corner Arc's release and the Roll-on are separate limits and both are needed. Making the release progressive removes the discontinuity but bounds nothing, because the freed grip can still arrive faster than any driver could use it.
