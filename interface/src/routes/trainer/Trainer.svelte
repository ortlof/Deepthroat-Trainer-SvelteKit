<script lang="ts">
	import { onMount, onDestroy } from 'svelte';
	import { page } from '$app/state';
	import { user } from '$lib/stores/user';
	import { socket } from '$lib/stores/socket';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Target from '~icons/tabler/target-arrow';
	import Play from '~icons/tabler/player-play';
	import Stop from '~icons/tabler/player-stop';
	import Flag from '~icons/tabler/flag';
	import Crosshair from '~icons/tabler/crosshair';
	import Sensor from '~icons/tabler/radar-2';
	import Device from '~icons/tabler/device-desktop-analytics';
	import Sliders from '~icons/tabler/adjustments-horizontal';

	interface TrainerState {
		session_start: boolean;
		session_stop: boolean;
		zero: boolean;
		reset_peak: boolean;
		goal_level: number;
		display_on: boolean;

		sensor_present: boolean;
		sensor_model_id: number;
		sensor_model: string;
		sensor_mode: string;
		sensor_status: number;
		sensor_status_text: string;
		range_mm: number;
		sensor_good: number;
		sensor_bad: number;

		depth_mm: number;
		depth_raw_mm: number;
		peak_mm: number;
		gauge: number;
		level: number;
		level_name: string;
		goal_mm: number;
		in_zone: boolean;
		hold_complete: boolean;
		hold_progress: number;
		hold_seconds_held: number;
		hold_seconds_needed: number;

		session_active: boolean;
		session_seconds: number;
		session_avg_mm: number;
		session_deep_seconds: number;
		session_holds: number;
		last_session_peak_mm: number;
		last_session_avg_mm: number;
		last_session_seconds: number;
		last_session_holds: number;
		last_session_at: number;

		display_present: boolean;
		display_on_state: boolean;
		display_brightness: number;
		display_frames: number;
		touch_present: boolean;
		touch_presses: number;
		battery_v: number;
		battery_known: boolean;

		units: number;
		reference_mm: number;
		auto_advance: boolean;
		notice: string;
	}

	interface TrainerSettings {
		reference_mm: number;
		max_depth_mm: number;
		calibration_mm: number;
		level1_mm: number;
		level2_mm: number;
		level3_mm: number;
		hold_seconds: number;
		auto_advance: boolean;
		filter_tau_s: number;
		session_stop_s: number;
		units: number;
		display_brightness: number;
		dim_brightness: number;
		display_timeout_s: number;
	}

	let state: TrainerState = $state({
		session_start: false,
		session_stop: false,
		zero: false,
		reset_peak: false,
		goal_level: 0,
		display_on: true,
		sensor_present: false,
		sensor_model_id: 0,
		sensor_model: 'unknown',
		sensor_mode: 'absent',
		sensor_status: -1,
		sensor_status_text: 'no sample yet',
		range_mm: -1,
		sensor_good: 0,
		sensor_bad: 0,
		depth_mm: 0,
		depth_raw_mm: 0,
		peak_mm: 0,
		gauge: 0,
		level: 0,
		level_name: 'Warm up',
		goal_mm: 0,
		in_zone: false,
		hold_complete: false,
		hold_progress: 0,
		hold_seconds_held: 0,
		hold_seconds_needed: 3,
		session_active: false,
		session_seconds: 0,
		session_avg_mm: 0,
		session_deep_seconds: 0,
		session_holds: 0,
		last_session_peak_mm: 0,
		last_session_avg_mm: 0,
		last_session_seconds: 0,
		last_session_holds: 0,
		last_session_at: 0,
		display_present: false,
		display_on_state: true,
		display_brightness: 70,
		display_frames: 0,
		touch_present: false,
		touch_presses: 0,
		battery_v: 0,
		battery_known: false,
		units: 0,
		reference_mm: 150,
		auto_advance: false,
		notice: ''
	});

	let settings: TrainerSettings = $state({
		reference_mm: 150,
		max_depth_mm: 120,
		calibration_mm: 0,
		level1_mm: 40,
		level2_mm: 70,
		level3_mm: 100,
		hold_seconds: 3,
		auto_advance: false,
		filter_tau_s: 0.2,
		session_stop_s: 8,
		units: 0,
		display_brightness: 70,
		dim_brightness: 10,
		display_timeout_s: 0
	});

	const UNIT_NAMES = ['mm', 'cm', 'in'];

	/* The ring: 270 degrees of a circle, drawn with a dash pattern. */
	const RING_RADIUS = 90;
	const RING_CIRCUMFERENCE = 2 * Math.PI * RING_RADIUS;
	const RING_ARC = RING_CIRCUMFERENCE * 0.75;

	let ringValue = $derived(Math.max(0, Math.min(1, state.gauge)) * RING_ARC);
	let ringPeak = $derived(Math.max(0, Math.min(1, state.peak_mm / (settings.max_depth_mm || 1))) * RING_ARC);
	let ringGoal = $derived(
		settings.max_depth_mm > 0 ? Math.max(0, Math.min(1, state.goal_mm / settings.max_depth_mm)) * RING_ARC : 0
	);
	/* -135 puts the start of the dash pattern at the lower left, and the arc then
	 * runs clockwise over the top, the same way the panel draws it. */
	let peakAngle = $derived(135 + 270 * (settings.max_depth_mm > 0 ? state.peak_mm / settings.max_depth_mm : 0));
	let goalAngle = $derived(135 + 270 * (settings.max_depth_mm > 0 ? state.goal_mm / settings.max_depth_mm : 0));
	let displayDepth = $derived(
		state.units === 1 ? state.depth_mm / 10 : state.units === 2 ? state.depth_mm / 25.4 : state.depth_mm
	);
	let displayDepthText = $derived(state.units === 0 ? displayDepth.toFixed(0) : displayDepth.toFixed(1));

	function authHeader() {
		return {
			Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
			'Content-Type': 'application/json'
		};
	}

	async function loadState() {
		try {
			const response = await fetch('/rest/trainerState', { method: 'GET', headers: authHeader() });
			if (response.ok) state = await response.json();
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function loadSettings() {
		try {
			const response = await fetch('/rest/trainerSettings', { method: 'GET', headers: authHeader() });
			if (response.ok) settings = await response.json();
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function command(patch: Record<string, unknown>) {
		try {
			const response = await fetch('/rest/trainerState', {
				method: 'POST',
				headers: authHeader(),
				body: JSON.stringify(patch)
			});
			if (response.ok) {
				state = await response.json();
			} else {
				notifications.error('Command rejected (' + response.status + ')', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function saveSettings(patch: Partial<TrainerSettings>) {
		try {
			const response = await fetch('/rest/trainerSettings', {
				method: 'POST',
				headers: authHeader(),
				body: JSON.stringify(patch)
			});
			if (response.ok) {
				settings = await response.json();
			} else {
				notifications.error('Setting rejected (' + response.status + ')', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	function formatSeconds(value: number) {
		const total = Math.max(0, Math.round(value));
		const minutes = Math.floor(total / 60);
		const seconds = total % 60;
		return minutes + ':' + String(seconds).padStart(2, '0');
	}

	onMount(() => {
		loadState();
		loadSettings();
		socket.on<TrainerState>('trainer', (data) => {
			state = data;
		});
	});

	onDestroy(() => {
		socket.off('trainer');
	});
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Target class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Trainer</span>
	{/snippet}

	<div class="flex w-full flex-col items-center gap-6 md:flex-row md:items-start">
		<!-- The same ring the panel draws: 270 degrees, peak and goal marked. -->
		<svg viewBox="0 0 220 220" class="h-56 w-56 shrink-0">
			<g transform="rotate(135 110 110)">
				<circle
					cx="110"
					cy="110"
					r={RING_RADIUS}
					fill="none"
					stroke="currentColor"
					stroke-opacity="0.15"
					stroke-width="16"
					stroke-linecap="round"
					stroke-dasharray="{RING_ARC} {RING_CIRCUMFERENCE}"
				/>
				<circle
					cx="110"
					cy="110"
					r={RING_RADIUS}
					fill="none"
					stroke={state.in_zone ? 'var(--color-success)' : 'var(--color-info)'}
					stroke-width="16"
					stroke-linecap="round"
					stroke-dasharray="{ringValue} {RING_CIRCUMFERENCE}"
				/>
			</g>
			{#if state.goal_mm > 0}
				<line
					x1="110"
					y1="8"
					x2="110"
					y2="24"
					stroke="var(--color-warning)"
					stroke-width="4"
					transform="rotate({goalAngle} 110 110)"
				/>
			{/if}
			{#if state.peak_mm > 0}
				<line
					x1="110"
					y1="2"
					x2="110"
					y2="14"
					stroke="currentColor"
					stroke-width="3"
					transform="rotate({peakAngle} 110 110)"
				/>
			{/if}
			<text x="110" y="104" text-anchor="middle" class="fill-current text-6xl font-bold">
				{displayDepthText}
			</text>
			<text x="110" y="130" text-anchor="middle" class="fill-current text-xl">
				{UNIT_NAMES[state.units]}
			</text>
			<text x="110" y="154" text-anchor="middle" class="fill-current text-sm opacity-70">
				{state.level_name}
			</text>
		</svg>

		<div class="w-full space-y-4">
			<div class="stats stats-vertical shadow sm:stats-horizontal">
				<div class="stat">
					<div class="stat-title">Distance</div>
					<div class="stat-value text-2xl">
						{state.range_mm >= 0 ? state.range_mm.toFixed(0) + ' mm' : '--'}
					</div>
					<div class="stat-desc">
						{state.sensor_model}{state.sensor_model_id > 0 ? ' (0x' + state.sensor_model_id.toString(16).toUpperCase() + ')' : ''}
						&middot; {state.sensor_mode}
					</div>
					<div class="stat-desc">{state.sensor_status_text} ({state.sensor_status})</div>
				</div>
				<div class="stat">
					<div class="stat-title">Peak</div>
					<div class="stat-value text-2xl">{state.peak_mm.toFixed(0)} mm</div>
					<div class="stat-desc">session holds: {state.session_holds}</div>
				</div>
				<div class="stat">
					<div class="stat-title">{state.session_active ? 'Session' : 'Last session'}</div>
					<div class="stat-value text-2xl">
						{formatSeconds(state.session_active ? state.session_seconds : state.last_session_seconds)}
					</div>
					<div class="stat-desc">
						avg {(state.session_active ? state.session_avg_mm : state.last_session_avg_mm).toFixed(0)} mm &middot;
						peak
						{(state.session_active ? state.peak_mm : state.last_session_peak_mm).toFixed(0)} mm
					</div>
					<div class="stat-desc">
						{state.session_active
							? 'live'
							: state.last_session_holds + ' holds'}
					</div>
				</div>
			</div>

			{#if state.notice}
				<div class="alert alert-info py-2 shadow">{state.notice}</div>
			{/if}
			{#if !state.sensor_present}
				<div class="alert alert-error py-2 shadow">
					No distance sensor on the I2C bus (SDA {8}, SCL {48}, address 0x29). Check the wiring.
				</div>
			{/if}

			<div class="flex flex-wrap gap-2">
				<button
					class="btn btn-primary inline-flex items-center"
					disabled={state.session_active}
					onclick={() => command({ session_start: true })}
				>
					<Play class="mr-2 h-5 w-5" /><span>Start session</span>
				</button>
				<button
					class="btn btn-error inline-flex items-center"
					disabled={!state.session_active}
					onclick={() => command({ session_stop: true })}
				>
					<Stop class="mr-2 h-5 w-5" /><span>Stop session</span>
				</button>
				<button class="btn inline-flex items-center" onclick={() => command({ zero: true })}>
					<Crosshair class="mr-2 h-5 w-5" /><span>Zero here</span>
				</button>
				<button class="btn inline-flex items-center" onclick={() => command({ reset_peak: true })}>
					<Flag class="mr-2 h-5 w-5" /><span>Clear peak</span>
				</button>
			</div>

			<div class="divider">Goal</div>

			<div class="flex flex-wrap items-end gap-3">
				<div class="fieldset">
					<label class="label" for="goal">Level to hold</label>
					<select
						id="goal"
						class="select select-primary"
						value={state.goal_level}
						onchange={(event) => command({ goal_level: Number(event.currentTarget.value) })}
					>
						<option value={0}>Free play</option>
						<option value={1}>Level 1 ({settings.level1_mm.toFixed(0)} mm)</option>
						<option value={2}>Level 2 ({settings.level2_mm.toFixed(0)} mm)</option>
						<option value={3}>Level 3 ({settings.level3_mm.toFixed(0)} mm)</option>
					</select>
				</div>
				<div class="grow">
					<div class="label">
						Hold {state.hold_seconds_needed.toFixed(1)} s at {state.goal_mm.toFixed(0)} mm:
						{(state.hold_progress * 100).toFixed(0)} %
					</div>
					<progress
						class="progress progress-primary w-full"
						value={state.hold_progress * 100}
						max="100"
					></progress>
					<div class="label">
						{state.hold_complete
							? 'Hold complete'
							: state.in_zone
								? state.hold_seconds_held.toFixed(1) + ' s in the goal'
								: state.goal_level > 0
									? 'Go deeper than the goal to start the hold'
									: 'Pick a level, or leave it on free play'}
					</div>
				</div>
			</div>
		</div>
	</div>
</SettingsCard>

<SettingsCard collapsible={true} open={false}>
	{#snippet icon()}
		<Sliders class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Calibration and goals</span>
	{/snippet}

	<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
		<div class="fieldset">
			<label class="label" for="reference">
				Zero reference ({settings.reference_mm.toFixed(0)} mm)
			</label>
			<input
				id="reference"
				type="range"
				class="range range-primary"
				min="20"
				max="240"
				step="1"
				bind:value={settings.reference_mm}
				onchange={() => saveSettings({ reference_mm: settings.reference_mm })}
			/>
			<div class="label">
				The distance at which the depth counts as 0. Everything closer is depth.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="maxdepth">Full scale ({settings.max_depth_mm.toFixed(0)} mm)</label>
			<input
				id="maxdepth"
				type="range"
				class="range range-primary"
				min="10"
				max="240"
				step="5"
				bind:value={settings.max_depth_mm}
				onchange={() => saveSettings({ max_depth_mm: settings.max_depth_mm })}
			/>
			<div class="label">The ring is full at this depth, and the depth is clamped here.</div>
		</div>

		<div class="fieldset">
			<label class="label" for="calibration">
				Calibration ({settings.calibration_mm.toFixed(1)} mm)
			</label>
			<input
				id="calibration"
				type="range"
				class="range range-secondary"
				min="-60"
				max="60"
				step="0.5"
				bind:value={settings.calibration_mm}
				onchange={() => saveSettings({ calibration_mm: settings.calibration_mm })}
			/>
			<div class="label">
				Added to every distance. "Zero here" writes this value so the distance in front of the
				sensor means depth 0.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="tau">Smoothing ({settings.filter_tau_s.toFixed(2)} s)</label>
			<input
				id="tau"
				type="range"
				class="range range-secondary"
				min="0"
				max="1"
				step="0.05"
				bind:value={settings.filter_tau_s}
				onchange={() => saveSettings({ filter_tau_s: settings.filter_tau_s })}
			/>
			<div class="label">Time constant of the filter. 0 shows the raw sensor, which is noisy.</div>
		</div>

		<div class="fieldset">
			<label class="label" for="level1">Level 1 ({settings.level1_mm.toFixed(0)} mm)</label>
			<input
				id="level1"
				type="range"
				class="range range-primary"
				min="5"
				max="240"
				step="5"
				bind:value={settings.level1_mm}
				onchange={() => saveSettings({ level1_mm: settings.level1_mm })}
			/>
		</div>

		<div class="fieldset">
			<label class="label" for="level2">Level 2 ({settings.level2_mm.toFixed(0)} mm)</label>
			<input
				id="level2"
				type="range"
				class="range range-primary"
				min="5"
				max="240"
				step="5"
				bind:value={settings.level2_mm}
				onchange={() => saveSettings({ level2_mm: settings.level2_mm })}
			/>
		</div>

		<div class="fieldset">
			<label class="label" for="level3">Level 3 ({settings.level3_mm.toFixed(0)} mm)</label>
			<input
				id="level3"
				type="range"
				class="range range-primary"
				min="5"
				max="240"
				step="5"
				bind:value={settings.level3_mm}
				onchange={() => saveSettings({ level3_mm: settings.level3_mm })}
			/>
		</div>

		<div class="fieldset">
			<label class="label" for="hold">Hold time ({settings.hold_seconds.toFixed(1)} s)</label>
			<input
				id="hold"
				type="range"
				class="range range-primary"
				min="0.5"
				max="30"
				step="0.5"
				bind:value={settings.hold_seconds}
				onchange={() => saveSettings({ hold_seconds: settings.hold_seconds })}
			/>
			<div class="label">How long the goal must be held before it counts.</div>
		</div>

		<div class="fieldset">
			<label class="label cursor-pointer justify-start" for="autoadvance">
				<input
					id="autoadvance"
					type="checkbox"
					class="toggle toggle-primary mr-3"
					bind:checked={settings.auto_advance}
					onchange={() => saveSettings({ auto_advance: settings.auto_advance })}
				/>
				<span class="text-base">Climb a level after each completed hold</span>
			</label>
			<div class="label">Level 1 to 3, and it stops at 3.</div>
		</div>

		<div class="fieldset">
			<label class="label" for="stop">End a session after ({settings.session_stop_s.toFixed(0)} s)</label>
			<input
				id="stop"
				type="range"
				class="range range-secondary"
				min="2"
				max="60"
				step="1"
				bind:value={settings.session_stop_s}
				onchange={() => saveSettings({ session_stop_s: settings.session_stop_s })}
			/>
			<div class="label">No usable distance for this long ends the session and stores its totals.</div>
		</div>

		<div class="fieldset">
			<label class="label" for="units">Units</label>
			<select
				id="units"
				class="select select-primary w-full"
				value={settings.units}
				onchange={(event) => saveSettings({ units: Number(event.currentTarget.value) })}
			>
				<option value={0}>millimetres</option>
				<option value={1}>centimetres</option>
				<option value={2}>inches</option>
			</select>
		</div>
	</div>
</SettingsCard>

<SettingsCard collapsible={true} open={false}>
	{#snippet icon()}
		<Device class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Display</span>
	{/snippet}

	<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
		<div class="fieldset">
			<label class="label" for="brightness">Brightness ({settings.display_brightness} %)</label>
			<input
				id="brightness"
				type="range"
				class="range range-primary"
				min="1"
				max="100"
				step="1"
				bind:value={settings.display_brightness}
				onchange={() => saveSettings({ display_brightness: settings.display_brightness })}
			/>
			<div class="label">
				Panel: {state.display_present ? 'present' : 'not found'}, {state.display_frames} frames drawn
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="dim">Idle brightness ({settings.dim_brightness} %)</label>
			<input
				id="dim"
				type="range"
				class="range range-secondary"
				min="0"
				max="100"
				step="1"
				bind:value={settings.dim_brightness}
				onchange={() => saveSettings({ dim_brightness: settings.dim_brightness })}
			/>
		</div>

		<div class="fieldset">
			<label class="label" for="timeout">
				Dim after ({settings.display_timeout_s === 0 ? 'never' : settings.display_timeout_s + ' s'})
			</label>
			<input
				id="timeout"
				type="range"
				class="range range-secondary"
				min="0"
				max="600"
				step="10"
				bind:value={settings.display_timeout_s}
				onchange={() => saveSettings({ display_timeout_s: settings.display_timeout_s })}
			/>
			<div class="label">0 keeps the panel at full brightness. It never dims during a session.</div>
		</div>

		<div class="fieldset">
			<label class="label cursor-pointer justify-start" for="displayon">
				<input
					id="displayon"
					type="checkbox"
					class="toggle toggle-primary mr-3"
					bind:checked={state.display_on}
					onchange={() => command({ display_on: state.display_on })}
				/>
				<span class="text-base">Panel on</span>
			</label>
			<div class="label">
				Touch: {state.touch_present ? 'present' : 'not found'}, {state.touch_presses} presses. A tap
				starts or stops a session, a long press sets the zero reference.
			</div>
		</div>
	</div>
</SettingsCard>

<SettingsCard collapsible={true} open={false}>
	{#snippet icon()}
		<Sensor class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Sensor and board</span>
	{/snippet}

	<div class="stats stats-vertical shadow sm:stats-horizontal">
		<div class="stat">
			<div class="stat-title">Sensor</div>
			<div class="stat-value text-xl">{state.sensor_model}</div>
			<div class="stat-desc">model id 0x{state.sensor_model_id.toString(16).toUpperCase()}</div>
			<div class="stat-desc">{state.sensor_mode}</div>
		</div>
		<div class="stat">
			<div class="stat-title">Samples</div>
			<div class="stat-value text-xl">{state.sensor_good}</div>
			<div class="stat-desc">{state.sensor_bad} rejected</div>
			<div class="stat-desc">{state.sensor_status_text}</div>
		</div>
		<div class="stat">
			<div class="stat-title">Raw depth</div>
			<div class="stat-value text-xl">
				{state.depth_raw_mm >= 0 ? state.depth_raw_mm.toFixed(0) + ' mm' : '--'}
			</div>
			<div class="stat-desc">filtered {state.depth_mm.toFixed(1)} mm</div>
			<div class="stat-desc">reference {state.reference_mm.toFixed(0)} mm</div>
		</div>
		<div class="stat">
			<div class="stat-title">Battery</div>
			<div class="stat-value text-xl">
				{state.battery_known ? state.battery_v.toFixed(2) + ' V' : '--'}
			</div>
			<div class="stat-desc">meaningless while USB is connected</div>
		</div>
	</div>

	<div class="mt-4 text-sm opacity-70">
		The sensor sits on the same I2C bus as the touch controller and the display expander:
		SDA GPIO8, SCL GPIO48, address 0x29, 3.3 V. A second device on that bus at 0x29 is not
		possible without moving the touch controller.
	</div>
</SettingsCard>
