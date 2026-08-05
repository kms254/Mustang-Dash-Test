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

An airwire count also answers a narrower question than it appears to. It reports whether pads are *joined*, not whether any particular copper is what joins them — so on a net carrying a Copper Pour it cannot evaluate a change to that copper at all: the pour takes over and the count stays at zero. This cuts both ways and the second direction is easy to miss. Remove a dedicated connection and the pour absorbs it, so the count cannot say whether the removed copper was redundant or load-bearing. *Add* something — a via, a track — close enough to crowd a pad, and the pour reaches it through fewer spokes, which is a real change in resistance and current capacity that the count reports as nothing at all. Zero airwires is evidence about connectivity, never about whether copper was doing work.

### Copper Pour
A filled region of copper assigned to a net, poured around existing geometry rather than drawn as a path — the form the ground and supply nets take across most of a board's area. Distinct from a track, which joins two specific points, in that a pour joins everything it touches and reshapes itself on every refill.

Two consequences run through this project. A pour is *derived* geometry, so it is only true after a refill under the real Design Rules, and any measurement spanning an edit must refill first or it describes the previous board. And a pour silently substitutes: because it already touches every pad on its net, removing a dedicated connection to one of those pads does not disconnect it, it demotes it to a thermal-relief attachment — a real change in resistance and current capacity that connectivity checks report as no change at all.

A pour's inverse is a **rule area** (keepout): a region with the same outline semantics that bans something rather than filling it. Barring the pour from a region while still permitting tracks and vias is how a plane is held clear of a drilled hole without closing the area to routing.

Note that "keepout" is used in two senses here and only one of them is drawn. A rule area is a copper keepout the design tool enforces. A *mechanical* keepout — the space a cable, connector shell, or tool needs in order to mate — is a constraint on the physical product that no copper rule expresses, so nothing checks it unless someone draws a rule area specifically to stand in for it. The unqualified word usually means the copper one.

### Courtyard
The keep-clear region a footprint declares around itself, sized so that neighbouring parts can be placed and soldered without interference. It is an authored polygon belonging to the footprint, distinct from the pads (which are copper) and from the body outline (which is only the part's physical extent).

Two properties bite in practice. It is **frequently not rectangular**, so testing a point against its bounding box both over- and under-reports — anything asking "is this inside the keep-clear region" must test the polygon. And it is **authored, therefore omissible**: a footprint with no courtyard at all passes every courtyard check by having nothing to check, which is indistinguishable from passing on merit. A courtyard violation is also not automatically a defect; parts are sometimes deliberately placed tight, and the honest resolution is a scoped exception recording why rather than a suppressed check.

### Staged Rules
The practice of copying a board into scratch space beside a purpose-built set of sidecar files, and judging *that* copy, so the check runs against the project's real manufacturing constraints rather than whatever the design file happens to carry.

It exists because an imported board arrives with the importing tool's factory defaults rather than the fab's limits, and measuring against those produces hundreds of findings that are all false. The staging is therefore load-bearing rather than a convenience, and it has two failure modes worth knowing. **Stage too little** and the checker cannot resolve what it needs — a missing library table yields mass "not found" findings that also mask the real ones underneath, which is why a partial staging can only be trusted at the severity it was designed for. And because staging *overwrites* the copy's values for every rule it names, **the staged set outranks the design file**: a constraint tightened in the design and not mirrored into the staged set is silently relaxed for the duration of every check.

### Design Rules
The manufacturing constraints a board is drawn to — minimum clearance, track width, via diameter and drill, hole spacing — held as project metadata alongside the board rather than inside its geometry. Because they live outside the design objects, they travel separately: a tool can carry every footprint, net, and trace faithfully and still lose the rules entirely.

That separation is why rules are verified rather than assumed after any import or handoff. Measured against the wrong rules a correct board reports hundreds of violations, and a router reading those rules will produce copper the fab never agreed to — with nothing in either output indicating the constraints themselves are fiction. The tell is a violation set clustered just under a round number: real sloppiness scatters, a mismatched rule produces a band.

### Embedded Footprint Copy
Every placed footprint on a board is a full copy of its library definition, embedded in the board file at placement time — so the board and the library hold independent data for the same part, and only a field-level comparison keeps them honest.

Editing one copy never touches the other: a library-only fix changes nothing that gets fabricated, and an instance-only fix is silently reverted the next time the part is updated from the library, which is why footprint fixes here land in both. The copies are compared field-by-field in integer nanometres with pad-local overrides counted as data, so equal-looking values written at different decimal precision are genuinely different. When the copies disagree, the board copy is what gets fabricated, and reconciliation runs toward it.

### Coincident Connection
A connection made by two objects sharing an exact coordinate rather than by anything drawn between them — a net label placed directly on a pin, or a wire end landing on it. Most pins on this project's schematic are reached by a drawn wire, but a minority connect this way, and they cluster: whole sub-circuits exist in which no pin carries a wire at all.

It is genuinely connected and genuinely fragile, and the two facts are the same fact. Nothing records the *intent* to connect, so the relationship survives only while the coordinates stay equal: moving a symbol without moving everything that shares its pin coordinates silently disconnects it, and the schematic still looks right because the label is still there. Two consequences follow for any edit. Anything that relocates a part must carry its labels and wire ends by the same delta, as one operation. And placement off the connection grid, while not itself an error, removes the last thing that would make a later drag land back on the same point — so an off-grid coincident connection is a latent break rather than a cosmetic complaint.

### Embedded Symbol Copy
Every symbol placed on a schematic is a full copy of its library definition, embedded in the schematic file at placement time — the symbol-side counterpart of an Embedded Footprint Copy, and the reason a schematic keeps working long after its symbol library has stopped.

The consequence differs from the footprint case, and is worse. A footprint's two copies disagree; a symbol's embedded copy makes the library *unnecessary*. Drawing the sheet, exporting the netlist, building the BOM and checking the board all read the embedded copy, so the library on disk is consulted only while authoring — placing a new part, changing a symbol, updating fields from the library. Verification never touches it. A symbol library can therefore become entirely unloadable, taking every symbol in it down at once, while every downstream artefact stays correct and every check stays green; the failure surfaces only when someone next tries to author. Detecting it requires loading the libraries on purpose, because nothing that inspects the design will do it incidentally. The drift rule of the footprint copy still applies on top: an update-from-library rewrites instance fields from the library, so a divergence between the two is a silent revert waiting for its trigger.

### Warning Inbox
The project's treatment of design-check warnings as an inbox owed a decision: every warning is either fixed or given a scoped exemption carrying its recorded reason, and the inbox is held at zero so any standing warning is a signal rather than scenery.

The discipline exists because a warning that never clears gets waved through, and a per-item warning hides new defects behind an unchanging count — "pre-existing" is not a diagnosis. Exemption is reserved for geometry that is correct by intent; divergence between copies of the same data is drift and gets reconciled, not exempted.

Size defeats an inbox as surely as staleness does, and by a different route. Past some volume the report stops being read at all, and its total becomes a property attributed to the design — inherited noise, nothing to be done — rather than a queue anyone owns. A specific, correct, months-old finding survives indefinitely inside such a total, because nothing about it is hard except being seen. This is why the target is zero **wherever zero is reachable**: a count that only has to stop growing can never be read, while a report at zero makes the next single finding unmissable. Where a genuine inherited floor makes zero unreachable, a Ratcheted Gate is the honest instrument — but that floor is a claim about the artifact, and claims expire, so it is worth re-testing before it is renewed.

### Ratcheted Gate
A check that fails only on an *increase* measured against a recorded baseline, rather than on the presence of any finding at all — the form a gate takes when what it measures carries inherited noise that no correct change can clear. Its opposite is an absolute gate, which admits no baseline because a clean state is definable.

Choosing the wrong form fails in both directions, and neither failure is visible from the gate's own output. An absolute gate pointed at noisy output is unreachable, so it is either disabled or routinely overridden — the Warning Inbox problem arriving through the build instead of the tool. A ratcheted gate pointed at a condition with no legitimate baseline is worse, because it encodes the existing defect as the accepted floor and reports success while the defect stands. The question that settles the form is whether "zero" is a state this project could actually occupy; where it is, the gate is absolute. Where both forms are in use they stay separate steps rather than merging into one threshold, so each carries its own justification.

A ratchet is therefore a claim about the artifact, not a property of the check — and claims expire. An inherited floor is the usual justification, and floors get cleared; when that happens the ratchet keeps passing while asserting nothing, which is the failure this whole concept exists to name. Re-measure the floor before renewing a ratchet, and treat "we still need a baseline here" as a question with an answer rather than a settled fact.

Choosing the form is only half of it, because a gate of either form sees only what its severity filter admits. A check that runs, passes, and is structurally incapable of reporting the class it was meant to catch is more dangerous than an absent one, since a green result is read as evidence. So the filter belongs with the baseline as something the gate is audited on, rather than configuration that gets assumed once and inherited.

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
