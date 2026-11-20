// Map editor feature modules — include order matters (undo before features that push_undo).
#include "modules/mod_undo.inc.cpp"
#include "modules/mod_path_snap.inc.cpp"
#include "modules/mod_autofill_single.inc.cpp"
#include "modules/mod_autofill_path.inc.cpp"
#include "modules/mod_rotation.inc.cpp"
#include "modules/mod_adjust_hud.inc.cpp"
#include "modules/mod_gradients.inc.cpp"
#include "modules/mod_spline_chain_handles.inc.cpp"
