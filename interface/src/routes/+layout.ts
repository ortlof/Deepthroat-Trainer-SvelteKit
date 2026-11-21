import type { LayoutLoad } from './$types';

// This can be false if you're using a fallback (i.e. SPA mode)
export const prerender = false;
export const ssr = false;

export const load = (async ({ fetch }) => {
	const result = await fetch('/rest/features');
	const item = await result.json();
	return {
		features: item,
		title: 'DTT Trainer',
		github: 'ortlof/Deepthroat-Trainer-SvelteKit',
		copyright: '2026 ortlof',
		appName: 'DTT Trainer'
	};
}) satisfies LayoutLoad;
