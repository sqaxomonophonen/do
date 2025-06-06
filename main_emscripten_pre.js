
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
		// text input; "keydown" events don't work well for input methods like
		// dead keys, compose-key, pinyin input methods, and maybe also mobile
		// virtual keyboards? The new EditContext API ostensibly solves this
		// problem properly without any contenteditable hacks, but 1) It's
		// currently Chrome/Edge only, and 2) I managed to crash Chrome with a
		// tiny bit of EditContext code, so...
		const handle_text_input = Module.cwrap("handle_text_input", "undefined", ["string"]);
		const em_text_input_overlay = document.getElementById("text_input_overlay");
		em_text_input_overlay.addEventListener("input", () => {
			if (event.inputType === "insertText") {
				handle_text_input(event.data);
			}
			em_text_input_overlay.innerHTML = "";
		});
		em_text_input_overlay.focus();

		const em_drop = document.body;

		const set_drag_state = Module.cwrap("set_drag_state", "undefined", ["number"]);
		em_drop.addEventListener("dragenter", ()=>{set_drag_state(1);});
		//em_drop.addEventListener("dragover",  ()=>{set_drag_state(1);});
		em_drop.addEventListener("dragleave",  ()=>{set_drag_state(0);});
		em_drop.addEventListener("drop", ()=>{set_drag_state(0);});

		const heap_malloc = Module.cwrap("heap_malloc", "number", ["number"]);
		const handle_file_drop = Module.cwrap("handle_file_drop", "undefined", ["string", "number", "number"]);
		em_drop.addEventListener("drop", function() {
			const files = event.dataTransfer.files;
			for (let i=0; i<files.length; ++i) {
				const file = files[i];
				const r = new FileReader();
				((file) => {
					r.onload = function(e) {
						const b = new Uint8Array(e.target.result);
						// XXX "b" is on the stack, so we don't have room for
						// more than ~64kB it seems
						const p = heap_malloc(b.length);
						Module.HEAP8.set(b, p);
						console.log([file.name, b, p]);
						handle_file_drop(file.name, b.length, p);
					}
				})(file);
				r.readAsArrayBuffer(file);
			}
		}, false);

		console.log(Module);

		FS.mkdir('/data');

	}
];
