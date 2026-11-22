<script lang="ts">
	import { onMount } from 'svelte';
	import { page } from '$app/state';
	import { user } from '$lib/stores/user';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Ladder from '~icons/tabler/list-numbers';
	import Sliders from '~icons/tabler/adjustments-horizontal';
	import Sparkles from '~icons/tabler/sparkles';

	interface TrainerSettings {
		max_depth_mm: number;
		level_count: number;
		hold_seconds: number;
		auto_advance: boolean;
		level_count_down: boolean;
		play_mode: number;
		reps_target: number;
		praise: string;
		session_stop_s: number;
		[key: string]: number | boolean | string;
	}

	interface TrainerState {
		goal_level: number;
		depth_mm: number;
		level: number;
		level_name: string;
		in_zone: boolean;
		session_active: boolean;
		rep_count: number;
		reps_left: number;
		notice: string;
	}

	let settings: TrainerSettings = $state({
		max_depth_mm: 145,
		level_count: 5,
		hold_seconds: 3,
		auto_advance: false,
		level_count_down: false,
		play_mode: 0,
		reps_target: 5,
		praise: 'GOOD THING',
		session_stop_s: 8
	});

	let state: TrainerState = $state({
		goal_level: 0,
		depth_mm: 0,
		level: 0,
		level_name: 'Warm up',
		in_zone: false,
		session_active: false,
		rep_count: 0,
		reps_left: 0,
		notice: ''
	});

	const MAX_LEVELS = 10;
	const PRAISE_PICKS = ['GOOD GIRL', 'GOOD BOY', 'GOOD THING'];
	const COLOURS = [
		'#22d3ee',
		'#a78bfa',
		'#f472b6',
		'#facc15',
		'#4ade80',
		'#fb923c',
		'#60a5fa',
		'#f87171',
		'#34d399',
		'#c084fc'
	];

	/* The level list is edited here: how many levels the game has, and where each one sits on
	 * the measuring range. Everything the trainer does with the goals comes from this list. */
	function levelsInUse(): number[] {
		const count = Math.max(1, Math.min(MAX_LEVELS, Number(settings.level_count) || 1));
		return Array.from({ length: count }, (_, i) => i + 1);
	}

	function levelValue(level: number, which: 'mm' | 'max_mm'): number {
		return Number(settings[`level${level}_${which}`] ?? 0);
	}


	/* The band rail: 0 to 240 mm across, so both thumbs share one scale. */
	/* ------------------------------------------------------------ ladder builder ---
	 * Bands of one width, laid end to end from a starting depth, up to the travel the
	 * calibration measured. The whole ladder comes out of one button instead of ten slider
	 * pairs, and a shape worth keeping can be saved on the device. */
	function travelMm(): number {
		return Number(settings.max_depth_mm ?? 150);
	}
	function ladderWidth(): number {
		return Number(settings.ladder_width ?? 20);
	}
	function ladderBase(): number {
		return Number(settings.ladder_base ?? 0);
	}
	function ladderLevels(base = ladderBase(), width = ladderWidth()): number {
		return Math.min(10, Math.max(1, Math.floor((travelMm() - base + 0.001) / Math.max(1, width))));
	}
	function ladderPreview(): string {
		const base = ladderBase();
		const width = ladderWidth();
		const count = ladderLevels();
		const covered = Math.min(travelMm(), base + count * width);
		return `${count} bands of ${width} mm, ${base}-${covered} mm of ${travelMm()} mm travel`;
	}
	/* Exactly the same layout the firmware's ladderBuild() makes, so the panel and the page
	 * agree on what the button did. */
	function buildLadder(base = ladderBase(), width = ladderWidth()) {
		const count = ladderLevels(base, width);
		const patch: Record<string, number> = { level_count: count };
		for (let i = 0; i < 10; i++) {
			const on = i < count;
			patch[`level${i + 1}_mm`] = on ? base + i * width : 0;
			patch[`level${i + 1}_max_mm`] = on ? Math.min(travelMm(), base + (i + 1) * width) : 0;
		}
		saveSettings(patch as Partial<TrainerSettings>);
	}
	function presetCount(): number {
		return Number(settings.ladder_preset_count ?? 0);
	}
	function presetName(i: number): string {
		return String(settings[`ladder_preset${i + 1}_name`] ?? '');
	}
	function presetSummary(i: number): string {
		if (i >= presetCount()) return 'empty';
		return `${String(settings[`ladder_preset${i + 1}_levels`] ?? 0)} x ${String(settings[`ladder_preset${i + 1}_width`] ?? 0)} mm from ${String(settings[`ladder_preset${i + 1}_base`] ?? 0)}`;
	}
	function savePreset() {
		/* Into the next free slot, or over the last one once all four are used. */
		const slot = Math.min(presetCount(), 3);
		const patch: Record<string, string | number> = {
			[`ladder_preset${slot + 1}_name`]: presetNameField.slice(0, 15),
			[`ladder_preset${slot + 1}_base`]: ladderBase(),
			[`ladder_preset${slot + 1}_width`]: ladderWidth(),
			[`ladder_preset${slot + 1}_levels`]: ladderLevels()
		};
		if (slot >= presetCount()) patch.ladder_preset_count = slot + 1;
		saveSettings(patch as Partial<TrainerSettings>);
	}
	function loadPreset(i: number) {
		if (i >= presetCount()) return;
		const base = Number(settings[`ladder_preset${i + 1}_base`] ?? 0);
		const width = Number(settings[`ladder_preset${i + 1}_width`] ?? 20);
		saveSettings({ ladder_base: base, ladder_width: width } as Partial<TrainerSettings>);
		buildLadder(base, width);
	}

	/* A name for the shape worth keeping. */
	let presetNameField = $state('');

	function bandPct(value: number): number {
		return Math.max(0, Math.min(100, (Number(value) / 240) * 100));
	}

	const MIN_BAND = 10; /* the firmware refuses anything narrower, so the rail does too */

	/* Move one end of the band. The other end gives way rather than the band turning inside
	 * out or collapsing to nothing. Called on every pointer move so the rail follows the
	 * finger, and again when the thumb is let go, which is the call that writes to the
	 * device. */
	function setBand(level: number, which: 'low' | 'high', value: number, element?: HTMLInputElement) {
		let low = levelValue(level, 'mm');
		let high = levelValue(level, 'max_mm');
		if (which === 'low') {
			low = Math.max(0, Math.min(value, high - MIN_BAND));
		} else {
			high = Math.min(240, Math.max(value, low + MIN_BAND));
		}
		settings[`level${level}_mm`] = low;
		settings[`level${level}_max_mm`] = high;
		/* When the band refuses to follow the finger - both ends are already as close as they
		 * may come, or the device sends a narrower band back - the state is often unchanged, so
		 * nothing re-renders and the thumb would stay where it was let go. Put it back by hand. */
		const wanted = which === 'low' ? low : high;
		if (element && Number(element.value) !== wanted) {
			element.value = String(wanted);
		}
	}

	function saveBand(level: number, which: 'low' | 'high', value: number, element?: HTMLInputElement) {
		setBand(level, which, value, element);
		saveSettings({
			[`level${level}_mm`]: levelValue(level, 'mm'),
			[`level${level}_max_mm`]: levelValue(level, 'max_mm')
		} as Partial<TrainerSettings>);
	}

	function setLevelCount(count: number) {
		saveSettings({ level_count: Math.max(1, Math.min(MAX_LEVELS, count)) });
	}

	/* The ladder is drawn in the units the panel uses, so what is set here is what the ring
	 * shows. Depth 0 is the reference distance, full scale is max_depth_mm. */
	let span = $derived(Math.max(1, Number(settings.max_depth_mm) || 1));

	function barX(value: number): number {
		const mm = Math.max(0, Math.min(span, value));
		return 40 + (mm / span) * 540;
	}

	function barWidth(low: number, high: number): number {
		return Math.max(2, barX(high) - barX(low));
	}

	function authHeader() {
		return {
			Authorization: page.data.features.security ? 'Bearer ' + $user.bearer_token : 'Basic',
			'Content-Type': 'application/json'
		};
	}

	async function loadSettings() {
		try {
			const response = await fetch('/rest/trainerSettings', { method: 'GET', headers: authHeader() });
			if (response.ok) settings = { ...settings, ...(await response.json()) };
		} catch (error) {
			console.error('Error:', error);
		}
	}

	async function loadState() {
		try {
			const response = await fetch('/rest/trainerState', { method: 'GET', headers: authHeader() });
			if (response.ok) state = await response.json();
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
				settings = { ...settings, ...(await response.json()) };
			} else {
				notifications.error('Setting rejected (' + response.status + ')', 3000);
			}
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
			if (response.ok) state = await response.json();
		} catch (error) {
			console.error('Error:', error);
		}
	}

	onMount(() => {
		loadSettings();
		loadState();
		const timer = setInterval(loadState, 1000);
		return () => clearInterval(timer);
	});
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Ladder class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>The level ladder</span>
	{/snippet}

	<div class="fieldset">
		<label class="label" for="count">Levels in the game ({levelsInUse().length})</label>
		<input
			id="count"
			type="range"
			class="range range-primary"
			min="1"
			max={MAX_LEVELS}
			step="1"
			value={settings.level_count}
			onchange={(event) => setLevelCount(Number(event.currentTarget.value))}
		/>
		<div class="label">
			One to ten levels. Levels beyond this are kept but not used, so shrinking the game and
			growing it again does not lose the numbers.
		</div>
	</div>

	<!-- The ladder: every level's band on the measuring range, which is the same range the
	     ring shows. A glance here says whether the game is set up sanely. -->
	<div class="rounded-box bg-base-100 mt-2 p-3">
		<svg viewBox="0 0 {600} {26 + levelsInUse().length * 24}" class="w-full">
			<line x1="40" y1="14" x2="580" y2="14" stroke="currentColor" stroke-opacity="0.3" />
			{#each [0, 0.25, 0.5, 0.75, 1] as f}
				<line x1={40 + f * 540} y1="10" x2={40 + f * 540} y2="18" stroke="currentColor" stroke-opacity="0.3" />
				<text
					x={40 + f * 540}
					y="8"
					text-anchor="middle"
					class="fill-current"
					style="font-size:9px;opacity:0.7">{(span * f).toFixed(0)}</text
				>
			{/each}
			{#each levelsInUse() as level (level)}
				<text x="4" y={34 + (level - 1) * 24} class="fill-current" style="font-size:11px">
					L{level}
				</text>
				<rect
					x={barX(levelValue(level, 'mm'))}
					y={22 + (level - 1) * 24}
					width={barWidth(levelValue(level, 'mm'), levelValue(level, 'max_mm'))}
					height="14"
					rx="4"
					fill={COLOURS[(level - 1) % COLOURS.length]}
					opacity={state.goal_level === level ? 1 : 0.5}
				/>
				<text
					x={barX(levelValue(level, 'max_mm')) + 6}
					y={33 + (level - 1) * 24}
					class="fill-current"
					style="font-size:10px;opacity:0.8">
					{levelValue(level, 'mm').toFixed(0)}-{levelValue(level, 'max_mm').toFixed(0)}
				</text>
			{/each}
		</svg>
		<div class="label">
			Millimetres of depth, the same scale as the ring. The bright bar is the level the trainer is
			holding now ({state.goal_level === 0 ? 'none: free play' : 'level ' + state.goal_level}).
		</div>
	</div>

	<div class="mt-4 flex flex-wrap gap-2">
		{#each levelsInUse() as level (level)}
			<button
				class="btn btn-sm"
				class:btn-primary={state.goal_level === level}
				onclick={() => command({ goal_level: level })}>Hold level {level}</button
			>
		{/each}
		<button class="btn btn-sm" class:btn-primary={state.goal_level === 0} onclick={() => command({ goal_level: 0 })}
			>Free play</button
		>
	</div>
	<div class="label">
		Depth {state.depth_mm.toFixed(0)} mm, at {state.level_name}. In band: {state.in_zone ? 'yes' : 'no'}.
	</div>
</SettingsCard>

<SettingsCard collapsible={false} open={false}>
	{#snippet icon()}
		<Ladder class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Ladder builder</span>
	{/snippet}

	<div class="label">
		Bands of one width, laid end to end from a starting depth, across the travel the
		calibration measured. This is the same layout the panel's guide leaves behind.
	</div>

	<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
		<div class="fieldset">
			<label class="label" for="ladderWidth">Band width ({ladderWidth()} mm)</label>
			<input
				id="ladderWidth"
				type="range"
				class="range range-primary"
				min="5"
				max="60"
				step="1"
				value={ladderWidth()}
				onchange={(event) => saveSettings({ ladder_width: Number(event.currentTarget.value) } as Partial<TrainerSettings>)}
			/>
		</div>
		<div class="fieldset">
			<label class="label" for="ladderBase">First band starts at ({ladderBase()} mm)</label>
			<input
				id="ladderBase"
				type="range"
				class="range range-primary"
				min="0"
				max="60"
				step="5"
				value={ladderBase()}
				onchange={(event) => saveSettings({ ladder_base: Number(event.currentTarget.value) } as Partial<TrainerSettings>)}
			/>
		</div>
	</div>

	<div class="label font-medium">{ladderPreview()}</div>

	<div class="mt-2 flex flex-wrap items-center gap-2">
		<button class="btn btn-primary btn-sm" onclick={() => buildLadder()}>BUILD LADDER</button>
		<input
			class="input input-bordered input-sm w-40"
			type="text"
			placeholder="preset name"
			bind:value={presetNameField}
		/>
		<button class="btn btn-sm" onclick={savePreset}>SAVE PRESET</button>
	</div>

	<div class="mt-3 flex flex-wrap gap-2">
		{#each [0, 1, 2, 3] as slot (slot)}
			<button
				class="btn btn-sm {slot < presetCount() ? 'btn-secondary' : 'btn-ghost'}"
				disabled={slot >= presetCount()}
				onclick={() => loadPreset(slot)}
			>
				{presetName(slot) || `slot ${slot + 1}`} — {presetSummary(slot)}
			</button>
		{/each}
	</div>
</SettingsCard>

<SettingsCard collapsible={false} open={false}>
	{#snippet icon()}
		<Ladder class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Bands</span>
	{/snippet}

	<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
		{#each levelsInUse() as level (level)}
			{@const low = levelValue(level, 'mm')}
			{@const high = levelValue(level, 'max_mm')}
			<div class="fieldset">
				<label class="label" for="band{level}">
					<span class="badge badge-sm" style="background:{COLOURS[(level - 1) % COLOURS.length]}"></span>
					Level {level}: {low.toFixed(0)} to {high.toFixed(0)} mm
					<span class="opacity-60">({(high - low).toFixed(0)} mm wide)</span>
				</label>
				<!-- One rail, two thumbs: the level's band, dragged by either end. -->
				<div class="range-dual">
					<div class="range-dual-track">
						<div class="range-dual-fill" style="left:{bandPct(low)}%; right:{100 - bandPct(high)}%"></div>
					</div>
					<input
						id="band{level}"
						class="range-dual-input"
						type="range"
						min="0"
						max="240"
						step="5"
						aria-label="Level {level} starts at"
						value={low}
						oninput={(event) => setBand(level, 'low', Number(event.currentTarget.value), event.currentTarget)}
						onchange={(event) => saveBand(level, 'low', Number(event.currentTarget.value), event.currentTarget)}
					/>
					<input
						class="range-dual-input"
						type="range"
						min="0"
						max="240"
						step="5"
						aria-label="Level {level} counts up to"
						value={high}
						oninput={(event) => setBand(level, 'high', Number(event.currentTarget.value), event.currentTarget)}
						onchange={(event) => saveBand(level, 'high', Number(event.currentTarget.value), event.currentTarget)}
					/>
				</div>
				<div class="label">
					A goal counts only between these two depths. Past the upper limit it is not the goal. The
					two ends cannot come closer than ten millimetres, because a narrower band is a target
					nobody can hit.
				</div>
			</div>
		{/each}
	</div>
</SettingsCard>

<SettingsCard collapsible={false} open={false}>
	{#snippet icon()}
		<Sliders class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>How the game is played</span>
	{/snippet}

	<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
		<div class="fieldset">
			<label class="label" for="playmode">Play mode</label>
			<select
				id="playmode"
				class="select select-bordered"
				bind:value={settings.play_mode}
				onchange={() => saveSettings({ play_mode: Number(settings.play_mode) })}
			>
				<option value={0}>Hold the goal</option>
				<option value={1}>Count reps</option>
			</select>
			<div class="label">
				Reps: hold the goal for the hold time, then come fully back to 0. Each return counts one rep.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="order">Level order</label>
			<select
				id="order"
				class="select select-bordered"
				value={settings.level_count_down ? 1 : 0}
				onchange={(event) => saveSettings({ level_count_down: Number(event.currentTarget.value) === 1 })}
			>
				<option value={0}>Climb: level 1 up to level {levelsInUse().length}</option>
				<option value={1}>Count down: level {levelsInUse().length} back to level 1</option>
			</select>
			<div class="label">
				Which way a run walks through the levels. Counting down starts at the deepest level and
				works back to the easiest, and in rep mode with free play the run starts there too.
			</div>
		</div>

		<div class="fieldset">
			<label class="label cursor-pointer justify-start" for="advance">
				<input
					id="advance"
					type="checkbox"
					class="toggle toggle-primary mr-3"
					bind:checked={settings.auto_advance}
					onchange={() => saveSettings({ auto_advance: settings.auto_advance })}
				/>
				<span class="text-base">Move to the next level by itself</span>
			</label>
			<div class="label">
				Holding: after every completed hold. Reps: after every finished set, so a run is one set per
				level. The last level of the walk ends the run with the praise.
			</div>
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
				onchange={() => saveSettings({ hold_seconds: Number(settings.hold_seconds) })}
			/>
			<div class="label">How long the goal must be held before it counts. In rep mode the same time applies to every rep.</div>
		</div>

		<div class="fieldset">
			<label class="label" for="repstarget">
				Reps in a set ({settings.reps_target === 0 ? 'count up' : settings.reps_target + ', counting down'})
			</label>
			<input
				id="repstarget"
				type="range"
				class="range range-primary"
				min="0"
				max="50"
				step="1"
				bind:value={settings.reps_target}
				onchange={() => saveSettings({ reps_target: Number(settings.reps_target) })}
			/>
			<div class="label">
				0 counts up without a target. A number counts down from it, and a set ends when it is done.
				{state.session_active ? ' Running: ' + state.rep_count + ' done, ' + state.reps_left + ' to go.' : ''}
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="stop">End a run after ({Number(settings.session_stop_s).toFixed(0)} s without a reading)</label>
			<input
				id="stop"
				type="range"
				class="range range-secondary"
				min="2"
				max="60"
				step="1"
				bind:value={settings.session_stop_s}
				onchange={() => saveSettings({ session_stop_s: Number(settings.session_stop_s) })}
			/>
			<div class="label">
				No usable distance for this long ends the run and stores its totals.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="praise">Words when a run is finished</label>
			<input
				id="praise"
				type="text"
				maxlength="20"
				class="input input-bordered w-full uppercase"
				bind:value={settings.praise}
				onchange={() => saveSettings({ praise: String(settings.praise) })}
			/>
			<div class="flex flex-wrap gap-2 pt-2">
				{#each PRAISE_PICKS as pick}
					<button
						class="btn btn-sm"
						class:btn-primary={settings.praise === pick}
						onclick={() => {
							settings.praise = pick;
							saveSettings({ praise: pick });
						}}>{pick}</button
					>
				{/each}
			</div>
			<div class="label">
				Shown by the animation that ends a run. About twelve characters fit the panel.
			</div>
		</div>
	</div>

	<div class="mt-2 flex flex-wrap gap-2">
		<button class="btn btn-sm btn-primary inline-flex items-center" onclick={() => command({ session_start: true })}>
			<Sparkles class="mr-2 h-4 w-4" /><span>Start a run</span>
		</button>
		<button class="btn btn-sm inline-flex items-center" onclick={() => command({ session_stop: true })}>
			<span>Stop it</span>
		</button>
		<button class="btn btn-sm" onclick={() => command({ fx: 2, fx_count: 10, fx_text: settings.praise })}>
			Preview the praise
		</button>
	</div>
	<div class="label">{state.notice ? state.notice : 'The trainer reports here.'}</div>
</SettingsCard>
