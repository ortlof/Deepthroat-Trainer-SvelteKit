<script lang="ts">
	import type { PageData } from './$types';
	import logo from '$lib/assets/logo.png';
	import { notifications } from '$lib/components/toasts/notifications';

	interface Props {
		data: PageData;
	}

	let { data }: Props = $props();
</script>

<div class="hero bg-base-100 h-screen">
	<div class="card md:card-side bg-base-200 shadow-primary shadow-2xl">
		<figure class="bg-base-200"><img src={logo} alt="Logo" class="h-auto w-64" /></figure>
		<div class="card-body w-80">
			<h2 class="card-title text-center text-2xl">Deepthroat Trainer</h2>
			<p class="py-6 text-center">
				A depth sensor, a round display and a SvelteKit front end on the LilyGo T-RGB. The
				device measures with a VL6180X time-of-flight sensor and shows the depth as a ring.
				The web UI has the same readout, the calibration and the session totals. Built on
				<a href="https://github.com/theelims/ESP32-sveltekit" class="link" target="_blank" rel="noopener noreferrer"
					>ESP32-SvelteKit</a
				>.
			</p>
			<a
				class="btn btn-primary"
				href="/trainer"
				onclick={() => notifications.success('You did it!', 1000)}>Open the trainer</a
			>
		</div>
	</div>
</div>
