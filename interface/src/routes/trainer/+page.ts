import type { PageLoad } from './$types';

export const load = (async ({ fetch }) => {
	return {
		title: 'Trainer'
	};
}) satisfies PageLoad;
