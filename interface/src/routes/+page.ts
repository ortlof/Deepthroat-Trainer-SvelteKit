import { redirect } from '@sveltejs/kit';

/* The start page is gone. The trainer is what this device is for, so the address of the
 * device is the trainer. */
export const load = () => {
	throw redirect(307, '/trainer');
};
