<script lang="ts">
	import { onMount } from 'svelte';
	import { page } from '$app/state';
	import { user } from '$lib/stores/user';
	import { notifications } from '$lib/components/toasts/notifications';
	import SettingsCard from '$lib/components/SettingsCard.svelte';
	import Sliders from '~icons/tabler/adjustments-horizontal';
	import Ruler from '~icons/tabler/ruler-measure';
	import Sparkles from '~icons/tabler/sparkles';

	interface TrainerSettings {
		reference_mm: number;
		max_depth_mm: number;
		calibration_mm: number;
		level1_mm: number;
		level2_mm: number;
		level3_mm: number;
		level4_mm: number;
		level5_mm: number;
		level1_max_mm: number;
		level2_max_mm: number;
		level3_max_mm: number;
		level4_max_mm: number;
		level5_max_mm: number;
		hold_seconds: number;
		auto_advance: boolean;
		play_mode: number;
		reps_target: number;
		praise: string;
		filter_tau_s: number;
		session_stop_s: number;
		units: number;
		display_brightness: number;
		dim_brightness: number;
		display_timeout_s: number;
		panel_centre_x: number;
		panel_centre_y: number;
	}

	interface TrainerState {
		goal_level: number;
		rep_count: number;
		rep_target: number;
		depth_raw_mm: number;
		depth_mm: number;
		reference_mm: number;
		session_active: boolean;
	}

	let settings: TrainerSettings = $state({
		reference_mm: 150,
		max_depth_mm: 145,
		calibration_mm: 0,
		level1_mm: 40,
		level2_mm: 60,
		level3_mm: 80,
		level4_mm: 100,
		level5_mm: 120,
		level1_max_mm: 60,
		level2_max_mm: 80,
		level3_max_mm: 100,
		level4_max_mm: 120,
		level5_max_mm: 140,
		hold_seconds: 3,
		auto_advance: false,
		play_mode: 0,
		reps_target: 0,
		praise: 'GOOD THING',
		filter_tau_s: 0.2,
		session_stop_s: 8,
		units: 0,
		display_brightness: 70,
		dim_brightness: 10,
		display_timeout_s: 0,
		panel_centre_x: 225,
		panel_centre_y: 231
	});

	let state: TrainerState = $state({
		goal_level: 0,
		rep_count: 0,
		rep_target: 0,
		depth_raw_mm: -1,
		depth_mm: 0,
		reference_mm: 150,
		session_active: false
	});

	/* The two animations the trainer plays by itself. The names here are the quick
	 * picks the panel is sized for; anything shorter fits too. */
	const PRAISE_PICKS = ['GOOD GIRL', 'GOOD BOY', 'GOOD THING'];

	/* A level is a band: it counts from its depth up to its own upper limit. */
	const LEVELS = [1, 2, 3, 4, 5];

	function levelValue(level: number, which: 'mm' | 'max_mm'): number {
		const key = `level${level}_${which}` as keyof TrainerSettings;
		return settings[key] as number;
	}

	function saveLevel(level: number, low: number, high: number) {
		saveSettings({ [`level${level}_mm`]: low, [`level${level}_max_mm`]: high } as Partial<TrainerSettings>);
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
			if (response.ok) settings = await response.json();
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
				settings = await response.json();
			} else {
				notifications.error('Setting rejected (' + response.status + ')', 3000);
			}
		} catch (error) {
			console.error('Error:', error);
		}
	}

	/* Play one of the panel animations now, so it can be seen without doing a rep. */
	async function playEffect(kind: number) {
		const patch =
			kind === 2
				? { fx: 2, fx_count: state.rep_count || 5, fx_text: settings.praise }
				: { fx: 1, fx_count: (state.rep_count || 0) + 1 };
		try {
			const response = await fetch('/rest/trainerState', {
				method: 'POST',
				headers: authHeader(),
				body: JSON.stringify(patch)
			});
			if (!response.ok) notifications.error('Command rejected (' + response.status + ')', 3000);
		} catch (error) {
			console.error('Error:', error);
		}
	}

	onMount(() => {
		loadSettings();
		loadState();
		const timer = setInterval(loadState, 2000);
		return () => clearInterval(timer);
	});
</script>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Ruler class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Calibration</span>
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
			<div class="label">
				Right now: {state.depth_raw_mm >= 0 ? state.depth_raw_mm.toFixed(0) + ' mm' : '--'} in front of
				the sensor, depth {state.depth_mm.toFixed(1)} mm.
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
				Added to every distance. "Zero here" on the trainer page writes this value so the distance in
				front of the sensor means depth 0.
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
			<div class="label">The panel and the live readout follow this.</div>
		</div>

		<div class="fieldset">
			<label class="label" for="centrex">Panel centre X ({settings.panel_centre_x})</label>
			<input
				id="centrex"
				type="range"
				class="range range-secondary"
				min="200"
				max="280"
				step="1"
				bind:value={settings.panel_centre_x}
				onchange={() => saveSettings({ panel_centre_x: settings.panel_centre_x })}
			/>
			<div class="label">
				The round window is glued on by hand and is not always centred on the 480 wide panel, so
				the whole layout is moved. The default came from measuring this board's window: about 15
				px left of the middle.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="centrey">Panel centre Y ({settings.panel_centre_y})</label>
			<input
				id="centrey"
				type="range"
				class="range range-secondary"
				min="200"
				max="280"
				step="1"
				bind:value={settings.panel_centre_y}
				onchange={() => saveSettings({ panel_centre_y: settings.panel_centre_y })}
			/>
			<div class="label">
				About 9 px above the middle on this board. Nudge either value until the ring looks centred
				in the round window; the panel moves as you let the slider go.
			</div>
		</div>
	</div>
</SettingsCard>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Sliders class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Goals and play</span>
	{/snippet}

	<div class="grid grid-cols-1 gap-x-8 gap-y-2 md:grid-cols-2">
		{#each LEVELS as level (level)}
			<div class="fieldset">
				<label class="label" for="level{level}">
					Level {level} from ({levelValue(level, 'mm').toFixed(0)} mm)
				</label>
				<input
					id="level{level}"
					type="range"
					class="range range-primary"
					min="5"
					max="240"
					step="5"
					value={levelValue(level, 'mm')}
					onchange={(event) => saveLevel(level, Number(event.currentTarget.value), levelValue(level, 'max_mm'))}
				/>
				<label class="label" for="level{level}max">
					Level {level} counts up to ({levelValue(level, 'max_mm').toFixed(0)} mm)
				</label>
				<input
					id="level{level}max"
					type="range"
					class="range range-secondary"
					min="5"
					max="240"
					step="5"
					value={levelValue(level, 'max_mm')}
					onchange={(event) => saveLevel(level, levelValue(level, 'mm'), Number(event.currentTarget.value))}
				/>
				<div class="label">
					A goal only counts between these two depths. Past the upper limit it is not the goal, which
					stops {"\u2018"}deeper is better{"\u2019"} from completing a level. The upper limit is never allowed
					below the level itself.
				</div>
			</div>
		{/each}

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
			<div class="label">How long the goal must be held before it counts. In rep mode the same time applies to every rep.</div>
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
			<label class="label" for="playmode">Play mode</label>
			<select
				id="playmode"
				class="select select-bordered"
				bind:value={settings.play_mode}
				onchange={() => saveSettings({ play_mode: settings.play_mode })}
			>
				<option value={0}>Hold the goal</option>
				<option value={1}>Count reps</option>
			</select>
			<div class="label">
				Reps: hold the goal for the hold time, then come fully back to 0. Each return counts one rep.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="repstarget">
				Reps in the set ({settings.reps_target === 0 ? 'count up' : settings.reps_target + ', counting down'})
			</label>
			<input
				id="repstarget"
				type="range"
				class="range range-primary"
				min="0"
				max="50"
				step="1"
				bind:value={settings.reps_target}
				onchange={() => saveSettings({ reps_target: settings.reps_target })}
			/>
			<div class="label">
				0 counts up without a target. A number counts down from it, and the set ends when it is done.
			</div>
		</div>

		<div class="fieldset">
			<label class="label" for="praise">Words when a set is finished</label>
			<input
				id="praise"
				type="text"
				maxlength="20"
				class="input input-bordered w-full uppercase"
				bind:value={settings.praise}
				onchange={() => saveSettings({ praise: settings.praise })}
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
				Shown by the animation that ends a set. About twelve characters fit the panel.
			</div>
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
	</div>
</SettingsCard>

<SettingsCard collapsible={false}>
	{#snippet icon()}
		<Sparkles class="mr-2 h-6 w-6 shrink-0 self-end" />
	{/snippet}
	{#snippet title()}
		<span>Panel animations</span>
	{/snippet}

	<div class="text-base">
		One rep plays the count with the reminder to come all the way back out. The last rep of a set
		plays the praise animation. Both play on the panel whether or not this page is open.
	</div>

	<div class="flex flex-wrap gap-2 pt-4">
		<button class="btn btn-primary inline-flex items-center" onclick={() => playEffect(1)}>
			<Sparkles class="mr-2 h-5 w-5" /><span>Play the rep animation</span>
		</button>
		<button class="btn inline-flex items-center" onclick={() => playEffect(2)}>
			<Sparkles class="mr-2 h-5 w-5" /><span>Play the praise animation</span>
		</button>
	</div>
	<div class="label">
		The praise animation says "{settings.praise}" and the count of the last set.
	</div>
</SettingsCard>
