/**
 * GC_AUTOSTART=2 builds: a scripted controller 1 for unattended Dolphin runs. It plays the game from
 * the title screen into a new file: title -> File Select -> new file (name entry) -> start -> prologue
 * (Lost Woods, the Clock Tower's underground as Deku Link, the Clock Tower interior) -> Clock Town. There
 * it first explores South Clock Town (HUD, the pause menu's pages, the stick, a spin attack, a talk with a
 * carpenter), then tours South, West, East (once through the Treasure Chest Shop's door) and North Clock
 * Town and the Great Fairy's fountain (opening the pause menu in South Clock Town) for as long as the run
 * lasts: the three days, the moon's fall, the new cycle.
 *
 * The script is a table of steps. After every game frame (Gc_InputScriptFrame, on the game thread, so
 * the game state it reads is consistent) the first step whose conditions hold and that is not done yet
 * becomes the active one and decides the input for the next frames; osContGetReadData merges that
 * input into controller 1 (Gc_InputScriptGet, on the padmgr thread).
 *
 * A step applies in one gamestate (and, in Play, one scene and room) once the gamestate has run for
 * `afterFrames` frames, and only while its GC_IF_* conditions hold (title demo or the player's game,
 * message box open, cutscene running, player in control, player form, pause menu open). It presses
 * buttons in pulses: buttons[0] for `hold` frames, `gap` frames of nothing, buttons[1], ..., then from
 * buttons[0] again, for as long as the step stays active. So nothing depends on exact frames: a press
 * that comes too early is simply repeated. A GC_FOR(n) step is done after it was active n frames in all
 * (its pulses continue across activations, so a one-time press stays one press). The stick can be held,
 * or used to move the player:
 *   GC_MOVE_TO      walk toward (x, z), the stick turned through the camera's yaw as the player reads it;
 *                   the step is done once the player is within `radius` (0: never; such a step ends when
 *                   its scene or room condition stops holding, e.g. after a door or an exit), or gives up
 *                   after GC_WALK_GIVE_UP_FRAMES. GC_LOOP(n) on a step makes it and the n steps before it
 *                   a patrol: when it is done, they all start over.
 *   GC_MOVE_FLOWER  Deku flower at (x, z): walk onto it, hold A to burrow and charge (a launch after A was
 *                   held 10+ frames glides), release, steer the glide toward (x2, z2), press A over it to
 *                   drop. Done after landing away from the flower; no launch, or a landing back next to
 *                   the flower, starts over (a fall into a pit voids out, which restarts the scene).
 *   GC_MOVE_TALK    walk to the nearest actor `actorId` at the player's height; once it offers to talk,
 *                   pulse A. Done when the step comes back after a message box was open.
 *   GC_MOVE_DOOR    walk to (x2, z2) in front of the door at (x, z), then toward the door; once the door
 *                   offers itself (player->doorType), pulse A. The door cutscene and scene change follow.
 * `doneWhen` GC_DONE_PAUSED ends a step once the pause menu is open (Start can be ignored, so it presses
 * until then). GC_SHOT(tag, n) logs "@shot <count>-<tag>" after the step was active n frames: run_dolphin.ps1
 * screenshots the render window when that line arrives, and the step (a GC_FOR one) holds the picture still.
 * Done flags, flower phases and GC_FOR counts are per gamestate (a scene change, a new day or a void-out
 * respawn starts a new Play gamestate, and with it the scene's steps from the first one). Tour flags
 * (GC_TOUR_*) last across gamestates: a step can require them set (GC_NEED) or clear (GC_UNLESS), and set
 * (GC_SETS) or clear (GC_CLEARS) them when it becomes active, which routes the Clock Town tour.
 *
 * Coordinates come from the scenes' collision data (extracted/<version>/assets/scenes/<scene>/<scene>.c:
 * floor polygons, exit polygons, spawn and actor positions).
 *
 * The active step and the flower phases are logged ("script: ..."), next to trace.c's Play events.
 */
#include "ultra64.h"
#include "stdbool.h"
#include "macros.h"
#include "z64play.h"
#include "z64player.h"
#include "z64camera.h"
#include "z64math.h"
#include "z64message.h"
#include "z64save.h"
#include "z64scene.h"
#include "gc_bridge.h"
#include "gc_game.h"

#define GC_ANY (-1)

// gGameStateOverlayTable indices, as in trace.c
#define GC_GS_PLAY 3
#define GC_GS_FILE_SELECT 5

// Conditions (GcScriptStep.cond): every bit set must hold
#define GC_IF_TITLE (1 << 0)  // File Select not reached yet: the title screen and its demo
#define GC_IF_GAME (1 << 1)   // File Select was reached: the player's own game
#define GC_IF_MSG (1 << 2)    // Play: a message box is open
#define GC_IF_NO_MSG (1 << 3) // Play: no message box
#define GC_IF_CS (1 << 4)     // Play: a cutscene runs, or the player is held by one
#define GC_IF_CTRL (1 << 5)   // Play: the player can move (no message, cutscene or scene transition)
#define GC_IF_HUMAN (1 << 6)  // Play: player form is human
#define GC_IF_DEKU (1 << 7)   // Play: player form is Deku
#define GC_IF_PAUSED (1 << 8) // Play: the pause menu is open (or opening/closing)
#define GC_IF_ABOVE (1 << 9)  // Play: the player is at or above height `aboveY`

// Stick use (GcScriptStep.move)
#define GC_MOVE_NONE 0   // leave the stick alone
#define GC_MOVE_STICK 1  // hold stickX/stickY
#define GC_MOVE_TO 2     // walk toward (x, z); done within `radius`
#define GC_MOVE_FLOWER 3 // Deku flower at (x, z), glide to (x2, z2)
#define GC_MOVE_TALK 4   // walk to the nearest actor `actorId`, talk to it (A once it offers to talk)
#define GC_MOVE_DOOR 5   // walk to (x2, z2) in front of the door at (x, z), then into it, A to open it

// Completion conditions (GcScriptStep.doneWhen), besides GC_FOR and the move's own
#define GC_DONE_PAUSED 1 // the pause menu is open (a Start press can be ignored, e.g. while Tatl talks)

#define GC_STICK_FULL 70 // N64 stick value for a full push (the game caps the magnitude at 60)
// A slow walk: the player subtracts the dead zone and 20 more, so much weaker pushes (about 30) only turn him
#define GC_STICK_WALK 40.0f

// GC_MOVE_FLOWER phases
#define GC_FLOWER_WALK 0
#define GC_FLOWER_CHARGE 1
#define GC_FLOWER_GLIDE 2
#define GC_FLOWER_NEAR_RADIUS 40    // a flower floor this close to the flower's position is that flower
#define GC_FLOWER_CHARGE_FRAMES 50  // A held: burrowing takes about 25 frames, the glide needs 10 more
#define GC_FLOWER_DROP_RADIUS 40    // A over the target ends the glide
#define GC_FLOWER_LEFT_RADIUS 200   // landed this far from the flower: the flight worked

typedef struct GcScriptStep {
    const char* label;
    s8 gameState;      // GC_GS_* or GC_ANY
    s16 scene;         // Play: SCENE_* or GC_ANY
    s8 room;           // Play: room number or GC_ANY
    u16 cond;          // GC_IF_*
    u16 afterFrames;   // frames the gamestate must have run
    u16 buttons[4];    // N64 buttons of successive pulses (0 ends the list); all 0: no buttons
    u8 hold;           // frames a pulse is held
    u8 gap;            // frames released after a pulse
    u8 move;           // GC_MOVE_*
    s8 stickX, stickY; // GC_MOVE_STICK
    s16 x, z;          // GC_MOVE_TO target, GC_MOVE_FLOWER flower
    s16 radius;        // GC_MOVE_TO: done within this distance
    s16 x2, z2;        // GC_MOVE_FLOWER glide target
    u8 loopBack;       // when done: also clear the done flags of this many steps before it (a patrol loop)
    u16 duration;      // done after it was active this many frames (0: no limit)
    s16 aboveY;        // GC_IF_ABOVE: minimum player height
    u8 needFlags;      // tour flags (GC_TOUR_*) that must be set
    u8 notFlags;       // tour flags that must be clear
    u8 setFlags;       // tour flags set when the step becomes active
    u8 clearFlags;     // tour flags cleared when the step becomes active
    s16 actorId;       // GC_MOVE_TALK: ACTOR_* to talk to
    u8 doneWhen;       // GC_DONE_*
    u16 shotAt;        // with `shot`: after the step was active this many frames (in this gamestate)...
    const char* shot;  // ...log "@shot <n>-<shot>": run_dolphin.ps1 screenshots the render window
} GcScriptStep;

// Tour flags: what the Clock Town tour did on its current lap. Unlike done flags they survive scene changes.
#define GC_TOUR_WEST (1 << 0)     // West Clock Town visited
#define GC_TOUR_FOUNTAIN (1 << 1) // the Great Fairy's fountain visited
// ...and what it did once, on the first visits
#define GC_TOUR_EXPLORED (1 << 2) // South Clock Town explored (HUD, pause menu pages, stick, talk to a carpenter)
#define GC_TOUR_TALKING (1 << 3)  // the talk of the exploration started (its message box gets a screenshot)
#define GC_TOUR_DOOR (1 << 4)     // been through the Treasure Chest Shop's door in East Clock Town

// A GC_MOVE_TO step that has not reached its point after this many frames gives up (marked done), so a player
// stuck behind something (an NPC, a wall the straight line did not account for) does not stall the script
#define GC_WALK_GIVE_UP_FRAMES 600

// A screenshot request holds the game thread this long (emulated time), see Gc_ScriptShot
#define GC_SHOT_HOLD_MS 1000

// Where a step applies
#define GC_IN_STATE(gs) .gameState = (gs), .scene = GC_ANY, .room = GC_ANY
#define GC_IN_PLAY .gameState = GC_GS_PLAY, .scene = GC_ANY, .room = GC_ANY
#define GC_IN_ROOM(sc, rm) .gameState = GC_GS_PLAY, .scene = (sc), .room = (rm)
// What it does
#define GC_PRESS(b0, b1, b2, b3, h, g) .buttons = { (b0), (b1), (b2), (b3) }, .hold = (h), .gap = (g)
#define GC_WALK(px, pz, r) .move = GC_MOVE_TO, .x = (px), .z = (pz), .radius = (r)
#define GC_FLOWER(fx, fz, tx, tz) .move = GC_MOVE_FLOWER, .x = (fx), .z = (fz), .x2 = (tx), .z2 = (tz)
#define GC_LOOP(n) .loopBack = (n)
#define GC_FOR(frames) .duration = (frames)
#define GC_ABOVE(y) .aboveY = (y)
#define GC_NEED(f) .needFlags = (f)
#define GC_UNLESS(f) .notFlags = (f)
#define GC_SETS(f) .setFlags = (f)
#define GC_CLEARS(f) .clearFlags = (f)
#define GC_STICK(sx, sy) .move = GC_MOVE_STICK, .stickX = (sx), .stickY = (sy)
#define GC_TALK(id) .move = GC_MOVE_TALK, .actorId = (id)
#define GC_DOOR(dx, dz, ax, az) .move = GC_MOVE_DOOR, .x = (dx), .z = (dz), .x2 = (ax), .z2 = (az)
#define GC_SHOT(tag, frame) .shot = (tag), .shotAt = (frame)

// The player can move, as Deku Link (everything after the transformation) or as human Link (the Lost Woods)
#define GC_DEKU_CTRL (GC_IF_GAME | GC_IF_CTRL | GC_IF_DEKU)
#define GC_HUMAN_CTRL (GC_IF_GAME | GC_IF_CTRL | GC_IF_HUMAN)

static const GcScriptStep sScript[] = {
    // Title screen and its demo: tap Start every 40 frames, as GC_AUTOSTART=1 does
    { "title: Start", GC_IN_STATE(GC_GS_PLAY), .cond = GC_IF_TITLE, .afterFrames = 60,
      GC_PRESS(START_BUTTON, 0, 0, 0, 4, 36) },
    // File Select (60 fps). Main menu, cursor on file 1 (empty): A opens name entry. Name entry: A types the
    // letter under the cursor ('A') twice, Start moves the cursor to END, A on END creates the file (saved to
    // flash) and returns to the main menu. A on file 1 (now named) opens it, A on "Yes" starts the game.
    // Start does what A does on the main menu and the confirm screen, so the same cycle works everywhere.
    { "file select: A A Start A", GC_IN_STATE(GC_GS_FILE_SELECT), .afterFrames = 40,
      GC_PRESS(A_BUTTON, A_BUTTON, START_BUTTON, A_BUTTON, 3, 12) },
    // Pause menu (opened by the steps further down). The first time (South Clock Town's exploration): a screenshot
    // of every page, R to the next one (Select Item, Map, Quest Status, Masks, back to Select Item), then Start.
    // The page turn takes about 20 frames; each page is held still for its screenshot.
    { "explore pause: select item", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_FOR(100), GC_SHOT("pause-item", 70) },
    { "explore pause: map", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_PRESS(R_TRIG, 0, 0, 0, 2, 120), GC_FOR(100), GC_SHOT("pause-map", 70) },
    { "explore pause: quest status", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_PRESS(R_TRIG, 0, 0, 0, 2, 120), GC_FOR(100), GC_SHOT("pause-quest", 70) },
    { "explore pause: masks", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_PRESS(R_TRIG, 0, 0, 0, 2, 120), GC_FOR(100), GC_SHOT("pause-masks", 70) },
    { "explore pause: back to select item", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED,
      GC_UNLESS(GC_TOUR_EXPLORED), GC_PRESS(R_TRIG, 0, 0, 0, 2, 120), GC_FOR(60) },
    { "explore pause: close", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_PRESS(START_BUTTON, 0, 0, 0, 3, 57) },
    // Afterwards: turn its pages with R (items, map, quest status, masks) for a while, then close it with Start
    { "pause: turn pages", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_PRESS(R_TRIG, 0, 0, 0, 2, 98),
      GC_FOR(500) },
    { "pause: close", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_PAUSED, GC_PRESS(START_BUTTON, 0, 0, 0, 3, 57) },
    // Clock Town tour bookkeeping (see the Clock Town steps below), on arrival in a scene: one frame each
    { "west clock town: arrived", GC_IN_ROOM(SCENE_ICHIBA, GC_ANY), .cond = GC_IF_GAME, GC_FOR(1),
      GC_SETS(GC_TOUR_WEST) },
    { "fairy fountain: arrived", GC_IN_ROOM(SCENE_YOUSEI_IZUMI, GC_ANY), .cond = GC_IF_GAME, GC_FOR(1),
      GC_SETS(GC_TOUR_FOUNTAIN) },
    { "east clock town: arrived, new lap", GC_IN_ROOM(SCENE_TOWN, GC_ANY), .cond = GC_IF_GAME, GC_FOR(1),
      GC_CLEARS(GC_TOUR_WEST | GC_TOUR_FOUNTAIN) },
    // In the game: advance every message box with A (the exploration's talk first gets a screenshot of its box)
    { "explore: message", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_MSG, GC_NEED(GC_TOUR_TALKING), GC_FOR(40),
      GC_SHOT("sct-talk", 30) },
    { "message: A", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_MSG, GC_PRESS(A_BUTTON, 0, 0, 0, 2, 4) },
    // Cutscenes without text: wait
    { "cutscene: wait", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_CS | GC_IF_NO_MSG },

    // Lost Woods (65), after Skull Kid took Epona: through the hollow log (EnHoll at 800 0 0 between rooms 0
    // and 1; aim past it, the room changes on the way), then to the scene exit (collision exit 1 around
    // 2540 190 -49), a fall into the Clock Tower's underground
    { "lost woods: to the log", GC_IN_ROOM(SCENE_LOST_WOODS, 0), .cond = GC_HUMAN_CTRL, GC_WALK(1000, 0, 0) },
    { "lost woods: to the exit", GC_IN_ROOM(SCENE_LOST_WOODS, 1), .cond = GC_HUMAN_CTRL, GC_WALK(2540, -49, 0) },

    // Clock Tower's underground (1A) as Deku Link. Room 0: from Skull Kid's stump north through the sliding
    // door (BgOpenShutter at 0 0 -645, opened with A), along the corridor to the east to the Deku flower at its
    // end (415 0 -1128; Tatl's flower tutorial plays on the way), glide over the dip (floor at -170; a launch
    // only rises about 65 units, the flowers in the dip are the way back up) to the east ledge, then the door
    // (DoorShutter 1230 0 -1140, A) into room 1.
    { "underground: through the door", GC_IN_ROOM(SCENE_OPENINGDAN, 0), .cond = GC_DEKU_CTRL,
      GC_PRESS(A_BUTTON, 0, 0, 0, 2, 6), GC_WALK(0, -800, 60) },
    { "underground: corridor corner", GC_IN_ROOM(SCENE_OPENINGDAN, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(25, -1150, 50) },
    { "underground: flower over the dip", GC_IN_ROOM(SCENE_OPENINGDAN, 0), .cond = GC_DEKU_CTRL,
      GC_FLOWER(415, -1128, 1000, -1150) },
    { "underground: door to room 1", GC_IN_ROOM(SCENE_OPENINGDAN, 0), .cond = GC_DEKU_CTRL,
      GC_PRESS(A_BUTTON, 0, 0, 0, 2, 6), GC_WALK(1400, -1140, 0) },
    // Room 1: platforms over a pit (a fall voids out to the door) between tree trunks, each with a Deku flower
    // whose glide reaches 600 units from the launch (a launch rises about 130, the glide then sinks slowly, and
    // the fall at the end of the range still carries forward). Platforms (floor polygons): entry x 1260..1590
    // z -1260..-1020; A x 1830..2070 z -1320..-1080; B (y 120) x 2190..2430 z -1440..-1200; C (y 120) x 2310..
    // 2490 z -960..-720; D x 1695..1842 z -585..-359; E x 1470..1710 z -240..-60 by the withered tree, then
    // north to the exit (collision exit 2 at 1590 0 120..600, to the Clock Tower interior). A, B and C (the
    // chest) are a detour: from C, trunks stand between it and D. The entry flower has a clear line to D's
    // south end (about 660 units).
    { "underground: flower to platform D", GC_IN_ROOM(SCENE_OPENINGDAN, 1), .cond = GC_DEKU_CTRL,
      GC_FLOWER(1500, -1140, 1785, -530) },
    { "underground: flower to platform E", GC_IN_ROOM(SCENE_OPENINGDAN, 1), .cond = GC_DEKU_CTRL,
      GC_FLOWER(1759, -421, 1600, -150) },
    { "underground: to the exit", GC_IN_ROOM(SCENE_OPENINGDAN, 1), .cond = GC_DEKU_CTRL, GC_WALK(1590, 600, 0) },

    // Clock Tower interior (63): from the corridor (room 1) through EnHoll (0 -300 -905) into the gear room
    // (room 0). Its floor (y -300) is below the upper level (y -30, the Happy Mask Salesman at -106 -30 -79);
    // ramps along the walls lead up: east (x 130..230) north from z -150 to 150, north (z 150..270) west to
    // x -150, west (x -270..-150) south to z -90. Then the stairs (x -80..80, z 90..445, up to y 45) to the door
    // (ObjTokeiTobira 0 45 288) and the exit to South Clock Town (collision exit 2 at 0 45 357). The upper-level
    // steps come first: they take over once the ramps are climbed, and they are where a new cycle starts (after
    // the moon's fall the game restarts on the upper level, by the Salesman).
    { "clock tower: corridor", GC_IN_ROOM(SCENE_INSIDETOWER, 1), .cond = GC_DEKU_CTRL, GC_WALK(0, -700, 0) },
    { "clock tower: foot of the stairs", GC_IN_ROOM(SCENE_INSIDETOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(-45), GC_WALK(0, 40, 40) },
    { "clock tower: to the door", GC_IN_ROOM(SCENE_INSIDETOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(-45), GC_WALK(0, 400, 0) },
    { "clock tower: ramp start", GC_IN_ROOM(SCENE_INSIDETOWER, 0), .cond = GC_DEKU_CTRL, GC_WALK(180, -200, 40) },
    { "clock tower: east ramp", GC_IN_ROOM(SCENE_INSIDETOWER, 0), .cond = GC_DEKU_CTRL, GC_WALK(180, 210, 40) },
    { "clock tower: north ramp", GC_IN_ROOM(SCENE_INSIDETOWER, 0), .cond = GC_DEKU_CTRL, GC_WALK(-210, 210, 40) },
    { "clock tower: west ramp", GC_IN_ROOM(SCENE_INSIDETOWER, 0), .cond = GC_DEKU_CTRL, GC_WALK(-210, -150, 40) },

    // Clock Town, first cycle, while the three days pass: a tour that visits South, West, South, East, North,
    // the Great Fairy's fountain, North and South Clock Town again, round after round. Each new day reloads
    // the scene Link is in (after the day's title), and the tour continues from there. A scene's steps start
    // over whenever it is entered; the tour flags (set on arrival in West Clock Town and in the fountain,
    // cleared on arrival in East Clock Town) tell which way to go next.
    //
    // South Clock Town (6F). Arrivals: the Clock Tower door (-278 0 -801) on the first morning; the passage
    // behind the tower from North Clock Town (spawn 4, -260 200 -1380); the upper west street from West Clock
    // Town (spawn 3, -862 100 -1094). Open the pause menu once per arrival. From the north passage (y 200):
    // east, south along x -50, down to the plaza (y 0). From the upper west street (y 100): east, then down the
    // ramp at x -550. In the plaza: the west side (a channel at x -750..-650, z -950..-450 lies lower); then,
    // before West Clock Town was visited, west to it (collision exit 6 at -1000..-832 0 -160..-40); after it,
    // around the carnival tower scaffolding (ObjTokeiTurret, an actor at -290 0 160) on its north side, to the
    // ramp up to East Clock Town (collision exit 8 at 347..720 0..100 120..360).
    //
    // First arrival (the first morning, at the Clock Tower's door): explore before the tour. Stand still (the clock
    // fades while the player moves) for a screenshot of the HUD, open the pause menu (its pages: see above), walk
    // with the stick held in the four directions, spin (B), then talk to the nearest of Mutoh's carpenters working
    // on the carnival tower (EnDaiku, on the ground at -440 0 207, 105 0 39 and -277 0 384, offers to talk within 100
    // units; Mutoh himself is only here on the last night).
    { "explore: HUD", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED), GC_FOR(60),
      GC_SHOT("sct-hud", 45) },
    { "explore: open the pause menu", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_EXPLORED), GC_PRESS(START_BUTTON, 0, 0, 0, 3, 40), GC_FOR(200), .doneWhen = GC_DONE_PAUSED },
    { "explore: stick forward", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_STICK(0, GC_STICK_FULL), GC_FOR(30), GC_SHOT("sct-walk", 25) },
    { "explore: stick right", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_STICK(GC_STICK_FULL, 0), GC_FOR(30) },
    { "explore: stick back", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_STICK(0, -GC_STICK_FULL), GC_FOR(30) },
    { "explore: stick left", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_STICK(-GC_STICK_FULL, 0), GC_FOR(30) },
    { "explore: spin attack", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED),
      GC_PRESS(B_BUTTON, 0, 0, 0, 3, 60), GC_FOR(40), GC_SHOT("sct-spin", 8) },
    { "explore: talk to a carpenter", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_EXPLORED), GC_TALK(ACTOR_EN_DAIKU), GC_SETS(GC_TOUR_TALKING) },
    { "explore: done", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_EXPLORED), GC_FOR(1),
      GC_SETS(GC_TOUR_EXPLORED), GC_CLEARS(GC_TOUR_TALKING) },
    // Every arrival: open the pause menu (retried until it opens)
    { "south clock town: pause menu", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, .afterFrames = 200,
      GC_PRESS(START_BUTTON, 0, 0, 0, 3, 40), GC_FOR(200), .doneWhen = GC_DONE_PAUSED },
    { "south clock town: north passage, east", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(150), GC_WALK(-50, -1300, 40) },
    { "south clock town: north passage, south", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(150), GC_WALK(-50, -850, 40) },
    { "south clock town: east of the tower", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(60), GC_UNLESS(GC_TOUR_WEST), GC_WALK(-250, -650, 40) },
    { "south clock town: upper west street", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(60), GC_NEED(GC_TOUR_WEST), GC_WALK(-600, -1100, 40) },
    { "south clock town: west ramp down", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(60), GC_NEED(GC_TOUR_WEST), GC_WALK(-550, -750, 40) },
    { "south clock town: west", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_WALK(-650, -400, 40) },
    { "south clock town: west street", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_WEST), GC_WALK(-750, -100, 40) },
    { "south clock town: to West Clock Town", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_WEST), GC_WALK(-950, -100, 0) },
    { "south clock town: north-west", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-650, 150, 40) },
    { "south clock town: north side", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-550, 320, 40) },
    { "south clock town: north-east", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL, GC_WALK(330, 320, 40) },
    { "south clock town: to East Clock Town", GC_IN_ROOM(SCENE_CLOCKTOWER, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(600, 240, 0) },

    // West Clock Town (6D), from South Clock Town (spawn 1, -1007 0 -107): up the street to the west (y 0 to
    // 150), north across the upper square (y 200), east into the passage to South Clock Town's upper west street
    // (collision exit 3 at -1200..-1000 100..200 -1208..-992).
    // The upper square first (from y 185): a new day reloads the scene where Link stands, and from up there the
    // street steps would walk him into the square's south wall
    { "west clock town: upper square, north", GC_IN_ROOM(SCENE_ICHIBA, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(185), GC_WALK(-1800, -1050, 40) },
    { "west clock town: upper square, to South Clock Town", GC_IN_ROOM(SCENE_ICHIBA, 0),
      .cond = GC_DEKU_CTRL | GC_IF_ABOVE, GC_ABOVE(185), GC_WALK(-1050, -1100, 0) },
    { "west clock town: street", GC_IN_ROOM(SCENE_ICHIBA, 0), .cond = GC_DEKU_CTRL, GC_WALK(-1300, -250, 40) },
    { "west clock town: up the street", GC_IN_ROOM(SCENE_ICHIBA, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-1650, -300, 40) },
    { "west clock town: square, south", GC_IN_ROOM(SCENE_ICHIBA, 0), .cond = GC_DEKU_CTRL, GC_WALK(-1800, -600, 40) },
    { "west clock town: square, north", GC_IN_ROOM(SCENE_ICHIBA, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-1800, -1050, 40) },
    { "west clock town: to South Clock Town", GC_IN_ROOM(SCENE_ICHIBA, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-1050, -1100, 0) },

    // East Clock Town (6C), from South Clock Town (spawn 1, 697 94 205): north across the square (y 100), up the
    // ramp at x 850..950 (z -900..-1300, to y 200), north along the upper street, west to the passage to North
    // Clock Town (collision exit 6 at 520..768 200 -1864..-1704).
    // A new day reloads the scene where Link stands: on the upper street (y 200), go on from there
    { "east clock town: upper street, north", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(190), GC_WALK(900, -1780, 40) },
    { "east clock town: upper street, to North Clock Town", GC_IN_ROOM(SCENE_TOWN, 0),
      .cond = GC_DEKU_CTRL | GC_IF_ABOVE, GC_ABOVE(190), GC_WALK(600, -1790, 0) },
    // The first time: through the door of the Treasure Chest Shop (EnDoor at 610 100 -140, turned 180 degrees, so its
    // outside is to the north; open from 06:00 to 22:00), a look inside, and out again by the same door
    { "east clock town: north of the shop", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_DOOR),
      GC_WALK(700, -200, 30) },
    { "east clock town: treasure chest shop door", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_DOOR), GC_DOOR(610, -140, 610, -190) },
    { "east clock town: square", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL, GC_WALK(900, -500, 40) },
    { "east clock town: foot of the ramp", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL, GC_WALK(900, -850, 40) },
    { "east clock town: top of the ramp", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL, GC_WALK(900, -1300, 40) },
    { "east clock town: upper street", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL, GC_WALK(900, -1780, 40) },
    { "east clock town: to North Clock Town", GC_IN_ROOM(SCENE_TOWN, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(600, -1790, 0) },

    // North Clock Town (6E). Arrivals: from East Clock Town (spawn 1, 562 193 -1785), and from the fountain
    // (spawn 3, -1033 292 -2008) on the terrace in the west (y 290). The terrace is walled off from the square;
    // the way up is the stairs at x -910..-790 (rising north from z -2130, y 200, to z -2290, y 250), then west
    // along the landing at z -2290 and south onto the terrace (rising to y 290). Before the fountain was
    // visited: along the south edge (y 190), up the stairs to the fountain's door (collision exit 4 at
    // -1291..-1117 243..284 -2073..-1953). After it: back down the stairs (the steps hand over by height), the
    // south edge, south into the passage to South Clock Town (collision exit 3 at -320..-200 200 -1599..-1280).
    { "north clock town: terrace, to the landing", GC_IN_ROOM(SCENE_BACKTOWN, 0),
      .cond = GC_DEKU_CTRL | GC_IF_ABOVE, GC_ABOVE(270), GC_NEED(GC_TOUR_FOUNTAIN), GC_WALK(-1000, -2280, 30) },
    { "north clock town: landing, to the stairs", GC_IN_ROOM(SCENE_BACKTOWN, 0),
      .cond = GC_DEKU_CTRL | GC_IF_ABOVE, GC_ABOVE(235), GC_NEED(GC_TOUR_FOUNTAIN), GC_WALK(-850, -2285, 30) },
    { "north clock town: down the stairs", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL | GC_IF_ABOVE,
      GC_ABOVE(205), GC_NEED(GC_TOUR_FOUNTAIN), GC_WALK(-850, -2120, 30) },
    { "north clock town: south edge", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-250, -1780, 40) },
    { "north clock town: to the stairs", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_FOUNTAIN), GC_WALK(-700, -2000, 40) },
    { "north clock town: foot of the stairs", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_FOUNTAIN), GC_WALK(-850, -2140, 30) },
    { "north clock town: up the stairs", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_FOUNTAIN), GC_WALK(-850, -2285, 30) },
    { "north clock town: landing", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_FOUNTAIN),
      GC_WALK(-1000, -2285, 30) },
    { "north clock town: terrace", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL, GC_UNLESS(GC_TOUR_FOUNTAIN),
      GC_WALK(-1000, -2080, 40) },
    { "north clock town: to the fountain", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL,
      GC_UNLESS(GC_TOUR_FOUNTAIN), GC_WALK(-1250, -2020, 0) },
    { "north clock town: to South Clock Town", GC_IN_ROOM(SCENE_BACKTOWN, 0), .cond = GC_DEKU_CTRL,
      GC_WALK(-260, -1300, 0) },

    // The Great Fairy's fountain (26), Clock Town's (spawn 0, 2400 20 360): the Great Fairy, scattered into
    // stray fairies, talks on arrival; look around for a few seconds, then leave by the door behind (collision
    // exit 1 at 2298..2498 20 384..857, to North Clock Town's terrace).
    { "fairy fountain: look", GC_IN_ROOM(SCENE_YOUSEI_IZUMI, GC_ANY), .cond = GC_DEKU_CTRL, GC_FOR(100) },
    { "fairy fountain: leave", GC_IN_ROOM(SCENE_YOUSEI_IZUMI, GC_ANY), .cond = GC_DEKU_CTRL,
      GC_WALK(2400, 700, 0) },

    // The Treasure Chest Shop (17), entered from East Clock Town (spawn 0, -70 0 161, facing into the shop): look,
    // then leave by the door behind (EnDoor at -70 0 100)
    { "treasure chest shop: look", GC_IN_ROOM(SCENE_TAKARAYA, GC_ANY), .cond = GC_DEKU_CTRL, GC_FOR(80),
      GC_SHOT("takaraya", 50), GC_SETS(GC_TOUR_DOOR) },
    { "treasure chest shop: leave", GC_IN_ROOM(SCENE_TAKARAYA, GC_ANY), .cond = GC_DEKU_CTRL,
      GC_DOOR(-70, 100, -70, 140) },

    // Player in control, nothing scripted for this place: stand still
    { "control: idle", GC_IN_PLAY, .cond = GC_IF_GAME | GC_IF_CTRL },
};

#define GC_SCRIPT_STEPS ARRAY_COUNT(sScript)

static u8 sStepDone[GC_SCRIPT_STEPS];
static u8 sStepPhase[GC_SCRIPT_STEPS];        // GC_MOVE_FLOWER, GC_MOVE_TALK and GC_MOVE_DOOR phase
static u16 sStepPhaseFrames[GC_SCRIPT_STEPS]; // frames the step was active in that phase
static u16 sStepTotalFrames[GC_SCRIPT_STEPS]; // frames active in this gamestate, over all activations
static u32 sStepMark[GC_SCRIPT_STEPS];        // GC_MOVE_TALK: sMsgFrames when A was first pressed
static s32 sActiveStep = -1;
static u32 sStepFrames;      // frames the active step has been active
static void* sLastGameState; // the gamestate the done flags belong to
static s32 sReachedFileSelect;
static u8 sTourFlags;  // GC_TOUR_*
static u32 sMsgFrames; // Play frames with a message box open, since boot
static u32 sShots;     // screenshots asked for

// Output for osContGetReadData: buttons << 16 | (u8)stickX << 8 | (u8)stickY, and the flags below.
// Each is a single aligned 32-bit word; the padmgr thread may combine one poll's pad with the previous
// poll's flags, which only delays a change by one poll.
static volatile u32 sOutPad;
static volatile u32 sOutFlags;
#define GC_OUT_ACTIVE (1 << 0)
#define GC_OUT_STICK (1 << 1)

/**
 * Whether the player can be moved now (no message box, cutscene, scene transition or player cutscene state).
 * player->csId is not checked: it keeps the talk cutscene's id (CS_ID_GLOBAL_TALK) after Tatl's hints, while the
 * player can already move.
 */
static s32 Gc_ScriptPlayerInControl(PlayState* play, Player* player) {
    return (player != NULL) && (play->msgCtx.msgMode == MSGMODE_NONE) && (play->csCtx.state == CS_STATE_IDLE) &&
           (play->transitionTrigger == TRANS_TRIGGER_OFF) && (play->transitionMode == TRANS_MODE_OFF) &&
           !(player->stateFlags1 & (PLAYER_STATE1_DEAD | PLAYER_STATE1_20000000)) && !IS_PAUSED(&play->pauseCtx);
}

static s32 Gc_ScriptStepApplies(const GcScriptStep* step, s32 gameState, u32 frames, PlayState* play) {
    u16 cond = step->cond;
    Player* player = NULL;

    if ((step->gameState != GC_ANY) && (step->gameState != gameState)) {
        return false;
    }
    if (frames < step->afterFrames) {
        return false;
    }
    if ((cond & GC_IF_TITLE) && sReachedFileSelect) {
        return false;
    }
    if (((sTourFlags & step->needFlags) != step->needFlags) || ((sTourFlags & step->notFlags) != 0)) {
        return false;
    }
    if ((cond & GC_IF_GAME) && !sReachedFileSelect) {
        return false;
    }

    if (gameState != GC_GS_PLAY) {
        // Play-only conditions never hold elsewhere
        return (step->scene == GC_ANY) && (step->room == GC_ANY) &&
               !(cond & (GC_IF_MSG | GC_IF_NO_MSG | GC_IF_CS | GC_IF_CTRL | GC_IF_HUMAN | GC_IF_DEKU | GC_IF_PAUSED |
                         GC_IF_ABOVE));
    }

    player = GET_PLAYER(play);
    if ((step->scene != GC_ANY) && (step->scene != play->sceneId)) {
        return false;
    }
    if ((step->room != GC_ANY) && (step->room != play->roomCtx.curRoom.num)) {
        return false;
    }
    if ((cond & GC_IF_MSG) && (play->msgCtx.msgMode == MSGMODE_NONE)) {
        return false;
    }
    if ((cond & GC_IF_NO_MSG) && (play->msgCtx.msgMode != MSGMODE_NONE)) {
        return false;
    }
    if ((cond & GC_IF_CS) && (play->csCtx.state == CS_STATE_IDLE) &&
        ((player == NULL) || !(player->stateFlags1 & PLAYER_STATE1_20000000))) {
        return false;
    }
    if ((cond & GC_IF_CTRL) && !Gc_ScriptPlayerInControl(play, player)) {
        return false;
    }
    if ((cond & GC_IF_HUMAN) && (gSaveContext.save.playerForm != PLAYER_FORM_HUMAN)) {
        return false;
    }
    if ((cond & GC_IF_DEKU) && (gSaveContext.save.playerForm != PLAYER_FORM_DEKU)) {
        return false;
    }
    if ((cond & GC_IF_PAUSED) && !IS_PAUSED(&play->pauseCtx)) {
        return false;
    }
    if ((cond & GC_IF_ABOVE) && ((player == NULL) || (player->actor.world.pos.y < step->aboveY))) {
        return false;
    }
    return true;
}

static f32 Gc_ScriptDistXZ(Player* player, s16 x, s16 z) {
    Vec3f target;

    target.x = x;
    target.y = player->actor.world.pos.y;
    target.z = z;
    return Math_Vec3f_DistXZ(&player->actor.world.pos, &target);
}

/** The stick that moves the player toward (x, z) at `magnitude` (the player turns the stick angle atan2(-x, y)
 *  by the camera's input yaw, Player_ProcessControlStick). */
static void Gc_ScriptStickToward(PlayState* play, Player* player, s16 x, s16 z, f32 magnitude, s8* stickX,
                                 s8* stickY) {
    Vec3f target;
    s16 stickYaw;

    target.x = x;
    target.y = player->actor.world.pos.y;
    target.z = z;
    stickYaw = Math_Vec3f_Yaw(&player->actor.world.pos, &target) - Camera_GetInputDirYaw(GET_ACTIVE_CAM(play));
    *stickX = (s8)(-Math_SinS(stickYaw) * magnitude);
    *stickY = (s8)(Math_CosS(stickYaw) * magnitude);
}

/** Mark a step done; the last step of a patrol loop restarts the loop instead. */
static void Gc_ScriptStepDone(s32 index) {
    s32 i;

    sStepDone[index] = true;
    if (sScript[index].loopBack != 0) {
        for (i = index - sScript[index].loopBack; i <= index; i++) {
            sStepDone[i] = false;
            sStepPhase[i] = 0;
            sStepPhaseFrames[i] = 0;
            sStepTotalFrames[i] = 0;
        }
    }
}

/**
 * Ask run_dolphin.ps1 for a screenshot ("@shot <n>-<tag>"; it captures the render window when the line arrives), then
 * hold the game thread for GC_SHOT_HOLD_MS so that the picture on screen is the frame just drawn. The wait is a busy
 * loop: Dolphin emulates it at full cost, so it lasts long enough in wall time for the capture also when the run has
 * no speed limit (an idle wait would be skipped through in a few milliseconds). Higher-priority threads (the
 * scheduler and its graphics task, audio, the pad manager) keep running and finish and show the frame.
 */
static void Gc_ScriptShot(const char* tag) {
    OSTime end;

    sShots++;
    gc_log("script: @shot %02u-%s", (unsigned)sShots, tag);
    end = osGetTime() + OS_USEC_TO_CYCLES(GC_SHOT_HOLD_MS * 1000ull);
    while (osGetTime() < end) {
    }
}

/** The actor `id` nearest to the player at about the player's height (not up on a scaffolding), in any category;
 *  NULL if there is none. */
static Actor* Gc_ScriptFindActor(PlayState* play, Player* player, s16 id) {
    Actor* best = NULL;
    f32 bestDist = 0.0f;
    s32 category;

    for (category = 0; category < ACTORCAT_MAX; category++) {
        Actor* actor;

        for (actor = play->actorCtx.actorLists[category].first; actor != NULL; actor = actor->next) {
            if ((actor->id == id) && (fabsf(actor->world.pos.y - player->actor.world.pos.y) < 60.0f)) {
                f32 dist = Math_Vec3f_DistXZ(&actor->world.pos, &player->actor.world.pos);

                if ((best == NULL) || (dist < bestDist)) {
                    best = actor;
                    bestDist = dist;
                }
            }
        }
    }
    return best;
}

/**
 * GC_MOVE_TALK: walk to the actor; once it offers to talk (player->talkActor, set by Actor_OfferTalk during the
 * frame and read by the player's next update), stand still and pulse A. The message box that opens is handled by
 * the message steps (they come first); the step is done when it becomes active again after a message was open.
 */
static void Gc_ScriptTalk(s32 index, PlayState* play, Player* player, u16* buttons, s8* stickX, s8* stickY) {
    const GcScriptStep* step = &sScript[index];
    Actor* target = Gc_ScriptFindActor(play, player, step->actorId);
    u16 frames = sStepPhaseFrames[index]++;

    if ((sStepPhase[index] == 1) && (sMsgFrames != sStepMark[index])) {
        gc_log("script: %s: talked (text %04X)", step->label, (unsigned)play->msgCtx.currentTextId);
        Gc_ScriptStepDone(index);
        return;
    }
    if ((target == NULL) || (sStepFrames >= GC_WALK_GIVE_UP_FRAMES)) {
        gc_log("script: gave up on %s at %d %d %d (%s)", step->label, (int)player->actor.world.pos.x,
               (int)player->actor.world.pos.y, (int)player->actor.world.pos.z,
               (target == NULL) ? "no such actor" : "no talk");
        Gc_ScriptStepDone(index);
        return;
    }
    if (player->talkActor == target) {
        if (sStepPhase[index] == 0) {
            sStepPhase[index] = 1;
            sStepMark[index] = sMsgFrames;
            frames = sStepPhaseFrames[index] = 0;
            gc_log("script: %s: offered at %d %d %d, %d from it", step->label, (int)player->actor.world.pos.x,
                   (int)player->actor.world.pos.y, (int)player->actor.world.pos.z, (int)target->xzDistToPlayer);
        }
        if ((frames % 6) < 2) {
            *buttons = A_BUTTON;
        }
    } else {
        Gc_ScriptStickToward(play, player, target->world.pos.x, target->world.pos.z,
                             CLAMP(target->xzDistToPlayer, GC_STICK_WALK, GC_STICK_FULL), stickX, stickY);
    }
}

/**
 * GC_MOVE_DOOR: walk to the point in front of the door, then toward the door at a walk; once the door offers itself
 * (EnDoor sets player->doorType when the player is within 50 units in front of it and faces it), stand still and
 * pulse A. Opening it starts the door cutscene and the scene change. Starts over if nothing happened in 200 frames.
 */
static void Gc_ScriptDoor(s32 index, PlayState* play, Player* player, u16* buttons, s8* stickX, s8* stickY) {
    const GcScriptStep* step = &sScript[index];
    u16 frames = sStepPhaseFrames[index]++;

    if (sStepFrames >= 3 * GC_WALK_GIVE_UP_FRAMES) {
        gc_log("script: gave up on %s at %d %d %d", step->label, (int)player->actor.world.pos.x,
               (int)player->actor.world.pos.y, (int)player->actor.world.pos.z);
        Gc_ScriptStepDone(index);
        return;
    }
    if (sStepPhase[index] == 0) {
        f32 dist = Gc_ScriptDistXZ(player, step->x2, step->z2);

        if (dist < 12.0f) {
            sStepPhase[index] = 1;
            sStepPhaseFrames[index] = 0;
            gc_log("script: %s: in front of the door at %d %d %d", step->label, (int)player->actor.world.pos.x,
                   (int)player->actor.world.pos.y, (int)player->actor.world.pos.z);
        } else {
            Gc_ScriptStickToward(play, player, step->x2, step->z2, CLAMP(dist, GC_STICK_WALK, GC_STICK_FULL), stickX,
                                 stickY);
        }
    } else if (player->doorType != PLAYER_DOORTYPE_NONE) {
        if ((frames % 6) < 2) {
            *buttons = A_BUTTON;
        }
    } else if (frames > 200) {
        sStepPhase[index] = 0;
        sStepPhaseFrames[index] = 0;
        gc_log("script: %s: the door did not open, again", step->label);
    } else {
        Gc_ScriptStickToward(play, player, step->x, step->z, GC_STICK_WALK, stickX, stickY);
    }
}

static void Gc_ScriptMoveTo(s32 index, PlayState* play, Player* player, s8* stickX, s8* stickY) {
    const GcScriptStep* step = &sScript[index];

    if (Gc_ScriptDistXZ(player, step->x, step->z) < step->radius) {
        gc_log("script: reached %s (%d %d %d)", step->label, (int)player->actor.world.pos.x,
               (int)player->actor.world.pos.y, (int)player->actor.world.pos.z);
        Gc_ScriptStepDone(index);
        return;
    }
    if ((step->radius != 0) && (sStepFrames >= GC_WALK_GIVE_UP_FRAMES)) {
        gc_log("script: gave up on %s at %d %d %d", step->label, (int)player->actor.world.pos.x,
               (int)player->actor.world.pos.y, (int)player->actor.world.pos.z);
        Gc_ScriptStepDone(index);
        return;
    }
    Gc_ScriptStickToward(play, player, step->x, step->z, GC_STICK_FULL, stickX, stickY);
    if ((sStepFrames % 40) == 39) {
        // Progress of long walks (a player stuck against something shows here)
        gc_log("script: %s: at %d %d %d yaw %04X speed %d, camera yaw %04X, stick %d %d", step->label,
               (int)player->actor.world.pos.x, (int)player->actor.world.pos.y, (int)player->actor.world.pos.z,
               (unsigned)(u16)player->actor.shape.rot.y, (int)player->speedXZ,
               (unsigned)(u16)Camera_GetInputDirYaw(GET_ACTIVE_CAM(play)), (int)*stickX, (int)*stickY);
    }
}

static void Gc_ScriptSetPhase(s32 index, s32 phase, Player* player) {
    static const char* const sPhaseNames[] = { "walk", "charge", "glide" };

    sStepPhase[index] = phase;
    sStepPhaseFrames[index] = 0;
    gc_log("script: %s: %s at %d %d %d", sScript[index].label, sPhaseNames[phase], (int)player->actor.world.pos.x,
           (int)player->actor.world.pos.y, (int)player->actor.world.pos.z);
}

static void Gc_ScriptFlower(s32 index, PlayState* play, Player* player, u16* buttons, s8* stickX, s8* stickY) {
    const GcScriptStep* step = &sScript[index];
    s32 onGround = (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    u16 frames = sStepPhaseFrames[index]++;
    f32 dist;

    switch (sStepPhase[index]) {
        case GC_FLOWER_WALK:
            dist = Gc_ScriptDistXZ(player, step->x, step->z);
            if (onGround && (dist < GC_FLOWER_NEAR_RADIUS) &&
                SurfaceType_IsFloorDekuFlower(&play->colCtx, player->actor.floorPoly, player->actor.floorBgId)) {
                // On the flower (what the player checks when A is pressed): burrow
                Gc_ScriptSetPhase(index, GC_FLOWER_CHARGE, player);
                *buttons = A_BUTTON;
            } else {
                // Slow down near the flower, but not below a walk (a slow step does not climb onto it)
                Gc_ScriptStickToward(play, player, step->x, step->z, CLAMP(dist, 35.0f, GC_STICK_FULL), stickX,
                                     stickY);
            }
            break;

        case GC_FLOWER_CHARGE:
            *buttons = A_BUTTON;
            if (frames >= GC_FLOWER_CHARGE_FRAMES) {
                Gc_ScriptSetPhase(index, GC_FLOWER_GLIDE, player);
                *buttons = 0;
            }
            break;

        case GC_FLOWER_GLIDE:
            dist = Gc_ScriptDistXZ(player, step->x2, step->z2);
            if (onGround && (frames > 20)) {
                if (Gc_ScriptDistXZ(player, step->x, step->z) > GC_FLOWER_LEFT_RADIUS) {
                    // Away from the flower's platform (a fall into a pit voids out instead)
                    sStepDone[index] = true;
                    gc_log("script: %s: landed at %d %d %d, %d from the target", step->label,
                           (int)player->actor.world.pos.x, (int)player->actor.world.pos.y,
                           (int)player->actor.world.pos.z, (int)dist);
                } else {
                    // No launch, or back on the same platform: start over
                    gc_log("script: %s: landed %d units from the target", step->label, (int)dist);
                    Gc_ScriptSetPhase(index, GC_FLOWER_WALK, player);
                }
            } else if (!onGround && (dist < GC_FLOWER_DROP_RADIUS)) {
                *buttons = A_BUTTON; // let go of the flower petals over the target
            } else {
                Gc_ScriptStickToward(play, player, step->x2, step->z2, GC_STICK_FULL, stickX, stickY);
            }
            break;
    }
}

void Gc_InputScriptFrame(void) {
    s32 gameState;
    u32 frames;
    void* state;
    s32 active = -1;
    s32 i;
    u16 buttons = 0;
    s8 stickX = 0;
    s8 stickY = 0;
    u32 flags = 0;

    if (Gc_AutoStartMode() != 2) {
        return;
    }

    state = Gc_TraceCurGameState(&gameState, &frames);
    if (state == NULL) {
        // Between gamestates: release everything. The next gamestate starts its steps afresh, even when malloc
        // places it where this one was (a void-out reloads the scene into the same block).
        sOutFlags = 0;
        sOutPad = 0;
        sLastGameState = NULL;
        return;
    }
    if (gameState == GC_GS_FILE_SELECT) {
        sReachedFileSelect = true;
    }
    if (state != sLastGameState) {
        sLastGameState = state;
        for (i = 0; i < (s32)GC_SCRIPT_STEPS; i++) {
            sStepDone[i] = false;
            sStepPhase[i] = 0;
            sStepPhaseFrames[i] = 0;
            sStepTotalFrames[i] = 0;
        }
        sActiveStep = -1;
    }
    if (gameState == GC_GS_PLAY) {
        PlayState* play = state;

        if (play->msgCtx.msgMode != MSGMODE_NONE) {
            sMsgFrames++;
        }
        // A step that waits for the pause menu (its Start presses may be ignored) is done once the menu is open
        if ((sActiveStep >= 0) && (sScript[sActiveStep].doneWhen == GC_DONE_PAUSED) && IS_PAUSED(&play->pauseCtx)) {
            gc_log("script: %s: paused", sScript[sActiveStep].label);
            Gc_ScriptStepDone(sActiveStep);
        }
    }

    for (i = 0; i < (s32)GC_SCRIPT_STEPS; i++) {
        if (!sStepDone[i] && Gc_ScriptStepApplies(&sScript[i], gameState, frames, state)) {
            active = i;
            break;
        }
    }

    if (active != sActiveStep) {
        sActiveStep = active;
        sStepFrames = 0;
        if (active >= 0) {
            gc_log("script: %s (gamestate %d frame %u)", sScript[active].label, (int)gameState, (unsigned)frames);
            sTourFlags = (sTourFlags | sScript[active].setFlags) & ~sScript[active].clearFlags;
        } else if (gameState == GC_GS_PLAY) {
            PlayState* play = state;
            Player* player = GET_PLAYER(play);

            gc_log("script: no step (frame %u): msg %d cs %d trans %d/%d player csId %d st %08X %08X %08X",
                   (unsigned)frames, (int)play->msgCtx.msgMode, (int)play->csCtx.state, (int)play->transitionTrigger,
                   (int)play->transitionMode, (player != NULL) ? (int)player->csId : -2,
                   (player != NULL) ? (unsigned)player->stateFlags1 : 0,
                   (player != NULL) ? (unsigned)player->stateFlags2 : 0,
                   (player != NULL) ? (unsigned)player->stateFlags3 : 0);
        }
    }

    if (active >= 0) {
        const GcScriptStep* step = &sScript[active];
        u32 period = step->hold + step->gap;
        u32 numButtons = 0;
        // A GC_FOR step continues its pulses where it left off when it becomes active again (a one-time
        // press stays one press); other steps start their pulses afresh
        u32 t = (step->duration != 0) ? sStepTotalFrames[active] : sStepFrames;

        while ((numButtons < ARRAY_COUNT(step->buttons)) && (step->buttons[numButtons] != 0)) {
            numButtons++;
        }
        if ((numButtons != 0) && ((t % period) < step->hold)) {
            buttons = step->buttons[(t / period) % numButtons];
        }

        if (step->move == GC_MOVE_STICK) {
            stickX = step->stickX;
            stickY = step->stickY;
            flags |= GC_OUT_STICK;
        } else if (step->move != GC_MOVE_NONE) {
            Player* player = (gameState == GC_GS_PLAY) ? GET_PLAYER((PlayState*)state) : NULL;

            if (player != NULL) {
                switch (step->move) {
                    case GC_MOVE_TO:
                        Gc_ScriptMoveTo(active, state, player, &stickX, &stickY);
                        break;
                    case GC_MOVE_FLOWER:
                        Gc_ScriptFlower(active, state, player, &buttons, &stickX, &stickY);
                        break;
                    case GC_MOVE_TALK:
                        Gc_ScriptTalk(active, state, player, &buttons, &stickX, &stickY);
                        break;
                    case GC_MOVE_DOOR:
                        Gc_ScriptDoor(active, state, player, &buttons, &stickX, &stickY);
                        break;
                    default:
                        break;
                }
            }
            flags |= GC_OUT_STICK;
        }
        flags |= GC_OUT_ACTIVE;
        sStepFrames++;
        if (sStepTotalFrames[active] != 0xFFFF) {
            sStepTotalFrames[active]++;
        }
        if ((step->shot != NULL) && (sStepTotalFrames[active] == step->shotAt)) {
            Gc_ScriptShot(step->shot);
        }
        if ((step->duration != 0) && (sStepTotalFrames[active] >= step->duration)) {
            gc_log("script: %s done after %u frames", step->label, (unsigned)sStepTotalFrames[active]);
            Gc_ScriptStepDone(active);
        }
    }

    sOutPad = ((u32)buttons << 16) | ((u32)(u8)stickX << 8) | (u8)stickY;
    sOutFlags = flags;
}

s32 Gc_InputScriptGet(u16* buttons, s8* stickX, s8* stickY, s32* stickSet) {
    u32 flags = sOutFlags;
    u32 pad = sOutPad;

    if (!(flags & GC_OUT_ACTIVE)) {
        return false;
    }
    *buttons = pad >> 16;
    *stickX = (s8)(pad >> 8);
    *stickY = (s8)pad;
    *stickSet = (flags & GC_OUT_STICK) != 0;
    return true;
}
