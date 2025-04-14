
Module["locateFile"] = (path, scriptDirectory) => {
	// application files (html/js/wasm) are patched in order to move/rename
	// the paths as desired (to serve them from /_static/ and to version
	// them). however, unless "locateFile" is overridden like this, it
	// attempts to do some path manipulation that corrupts request URLs
	return path;
}

Module["pasted_text"] = "";
Module["preInit"] = [
	() => {
		// don't preinit in workers (right?):
		if (typeof document === "undefined") return;

		const em_canvas = document.getElementById("canvas");
		GL.makeContextCurrent(GL.createContext(em_canvas,{
			alpha: false,
			depth: false,
			stencil: false,
			antialias: false,
		}));

		document.addEventListener('paste', () => {
			event.preventDefault();
			const data = event.clipboardData.getData("text/plain");
			Module["pasted_text"] = data;
		});

		// Using a contenteditable div overlay (see also do.html) to capture
		// text input ("keydown" events alone cannot properly deal with dead
		// keys, compose-key, and probably other input methods like pinyin
		// input methods). The EditContext API ostensibly solves the same
		// problem properly without any hacks, but 1) It's currently
		// Chrome/Edge only, 2) I managed to crash Chrome with a tiny bit of
		// EditContext code, so...
		const handle_text_input = Module.cwrap("handle_text_input", "undefined", ["string"]);
		const em_text_input_overlay = document.getElementById("text_input_overlay");
		em_text_input_overlay.addEventListener("input", () => {
			if (event.inputType === "insertText") {
				handle_text_input(event.data);
			}
			em_text_input_overlay.innerHTML = "";
		});
		em_text_input_overlay.focus();
	}
];
