/// path_test()

global.asserts_passed = 0;
global.asserts_total = 0;

function do_assert(condition) {
	global.asserts_total++;
	if (condition) {
		global.asserts_passed++;
		return true;
	}
	return false;
}

function assert_equals(actual, expected, text) {
	if (do_assert(actual == expected)) {
		show_debug_message("AssertEquals PASS: " + text);
	} else {
		var error_msg = "AssertEquals FAIL: " + text + " expected " + string(expected) + " but found " + string(actual);
		show_debug_message(error_msg);
	}
}	

function assert_almost(actual, expected, text) {
	if (do_assert(abs(actual - expected) < 0.1)) {
		show_debug_message("AssertAlmost PASS: " + text);
	} else {
		var error_msg = "AssertAlmost FAIL: " + text + " expected " + string(expected) + " but found " + string(actual);
		show_debug_message(error_msg);
	}
}

function assert(condition, text) {
	if (do_assert(condition)) {
		show_debug_message("Assertion PASS: " + text);
	} else {
		var error_msg = "Assertion FAIL: " + text;
		show_debug_message(error_msg);
	}
}

function populate_path_with_points(path, points, text) {
	for (var i = 0; i < array_length(points); i++) {
		var txt = text + "[" + string(i) + "]";
		path_add_point(path, points[i].x, points[i].y, points[i].speed);
		check_point(path, i, points[i], text);
	}
}

function validate_path_points(path, points, text) {
	for (var i = 0; i < array_length(points); i++) {
		var txt = text + "[" + string(i) + "]";
		check_point(path, i, points[i], txt);
	}
}

function check_point(path, n, point, text) {
	assert_almost(path_get_point_x(path, n), point.x, text + ".path_get_point_x");
	assert_almost(path_get_point_y(path, n), point.y, text + ".path_get_point_y");
	assert_almost(path_get_point_speed(path, n), point.speed, text + ".path_get_point_speed");
}

function points_flip(points) {
	// Find bounds
	var min_y = points[0].y;
	var max_y = points[0].y;

	for (var i = 1; i < array_length(points); i++)
	{
	    min_y = min(min_y, points[i].y);
	    max_y = max(max_y, points[i].y);
	}

	// Flip vertically
	for (var i = 0; i < array_length(points); i++)
	{
	    points[i].y = min_y + max_y - points[i].y;
	}
}

function points_mirror(points) {
	// Find x bounds
	var min_x = points[0].x;
	var max_x = points[0].x;

	for (var i = 1; i < array_length(points); i++)
	{
	    min_x = min(min_x, points[i].x);
	    max_x = max(max_x, points[i].x);
	}

	// Flip horizontally
	for (var i = 0; i < array_length(points); i++)
	{
	    points[i].x = min_x + max_x - points[i].x;
	}
}

function points_reverse(points)
{
    var count = array_length(points);

    for (var i = 0; i < count div 2; i++)
    {
        var temp = points[i];
        points[i] = points[count - 1 - i];
        points[count - 1 - i] = temp;
    }
}

function points_rotate(points, angle)
{
    // Find bounds
    var min_x = points[0].x;
    var max_x = points[0].x;
    var min_y = points[0].y;
    var max_y = points[0].y;

    for (var i = 1; i < array_length(points); i++)
    {
        min_x = min(min_x, points[i].x);
        max_x = max(max_x, points[i].x);
        min_y = min(min_y, points[i].y);
        max_y = max(max_y, points[i].y);
    }

    // Rotation centre
    var cx = (min_x + max_x) / 2;
    var cy = (min_y + max_y) / 2;

    var c = cos(angle);
    var s = sin(angle);

    for (var i = 0; i < array_length(points); i++)
    {
        var _x = points[i].x - cx;
        var _y = points[i].y - cy;

        // GameMaker coordinate system (clockwise)
        points[i].x = cx + _x * c + _y * s;
        points[i].y = cy - _x * s + _y * c;
    }
}

function points_rescale(points, xscale, yscale) {
    // Find bounds
    var min_x = points[0].x;
    var max_x = points[0].x;
    var min_y = points[0].y;
    var max_y = points[0].y;

    for (var i = 1; i < array_length(points); i++)
    {
        min_x = min(min_x, points[i].x);
        max_x = max(max_x, points[i].x);
        min_y = min(min_y, points[i].y);
        max_y = max(max_y, points[i].y);
    }

    // Path centre
    var cx = (min_x + max_x) / 2;
    var cy = (min_y + max_y) / 2;

    // Scale around centre
    for (var i = 0; i < array_length(points); i++)
    {
        points[i].x = cx + (points[i].x - cx) * xscale;
        points[i].y = cy + (points[i].y - cy) * yscale;
    }
}

function points_shift(points, xshift, yshift)
{
    for (var i = 0; i < array_length(points); i++)
    {
        points[i].x += xshift;
        points[i].y += yshift;
    }
}


p = path_add();

// Defaults test
assert_equals(path_get_closed(p), true, "defaults.path_get_closed");
assert_equals(path_get_kind(p), 0, "defaults.path_get_kind");
assert_equals(path_get_length(p), 0, "defaults.path_get_length");
assert_equals(path_get_name(p), "__newpath0", "defaults.path_get_name");
assert_equals(path_get_number(p), 0, "defaults.path_get_name");
assert_equals(path_get_precision(p), 4, "defaults.path_get_precision");
assert_equals(path_get_speed(p, 0), 100, "defaults.path_get_speed");
assert_equals(path_get_x(p, 0), 0, "defaults.path_get_x");
assert_equals(path_get_y(p, 0), 0, "defaults.path_get_y");
assert_equals(path_exists(p), 1, "defaults.path_exists");

// set_closed test
var set_closed_test = [false, true]
for (var i = 0; i < array_length(set_closed_test); i++) {
	var t = set_closed_test[i];
	path_set_closed(p, t);
	assert_equals(path_get_closed(p), t, "path_set_closed(" + string(t) + ")");
}

// set_kind test
var set_kind_test = [
	{ value: 1,  outcome: 1 },
	{ value: 2,  outcome: 0 },
	{ value: 0,  outcome: 0 },
	{ value: -1, outcome: 0 },
]
for (var i = 0; i < array_length(set_kind_test); i++) {
	var t = set_kind_test[i];
	path_set_kind(p, t.value);
	assert_equals(path_get_kind(p), t.outcome, "path_set_kind(" + string(t.value) + ")");
}

// set_precision test
var set_precision_test = [
	{ value: -1, outcome: 0 },
	{ value: "i", outcome: "i"},
	{ value: 9, outcome: 8 },
]
for (var i = 0; i < array_length(set_precision_test); i++) {
	var t = set_precision_test[i];
	switch(t.value) {
		case "i":
			// Docs say integer must be between 1 and 8, but 0 works fine too
			for (var j = 0; j < 9; j++) {
				path_set_precision(p, j);
				assert_equals(path_get_precision(p), j, "path_set_precision(" + string(j) + ")");
			}
			break;
		default:
			path_set_precision(p, t.value);
			assert_equals(path_get_precision(p), t.outcome, "path_set_precision(" + string(t.value) + ")");
			break;
	}
}
// Reset precision
path_set_precision(p, 4);

var points = [
    { x: 0,   y: 0,   speed: 100 },
    { x: 90,  y: 10,  speed: 200 },
    { x: 100, y: 110, speed: 300 }
];
populate_path_with_points(p, points, "populate_path_with_points");

// get_number test
assert_equals(path_get_number(p), array_length(points), "path_get_number");

// get_point_* test
for (var i = 0; i < array_length(points); i++) {
	check_point(p, i, points[i], "get_point_" + string(i) + "_");
}

// get_length test
var get_length_test = [
	{ closed: false, kind: 0, outcome: 191.05 },
	{ closed: false, kind: 1, outcome: 177.24 },
	{ closed: true,  kind: 0, outcome: 339.71 },
	{ closed: true,  kind: 1, outcome: 233.31 },
]
for (var i = 0; i < array_length(get_length_test); i++) {
	var t = get_length_test[i];
	path_set_closed(p, t.closed);
	path_set_kind(p, t.kind);
	assert_almost(path_get_length(p), t.outcome, "path_get_length(closed=" + string(t.closed) + ", kind=" + string(t.kind) + ")");
}

// Reset path to straight and closed
path_set_kind(p, 0);
path_set_closed(p, true);

// get_* test
var get_X_test = [
	{ f: 0.0,  x: 0.0,   y: 0.0,   speed: 100.0  },
	{ f: 0.25, x: 84.41, y: 9.38,  speed: 193.79 },
	{ f: 0.5,  x: 97.89, y: 88.91, speed: 278.91 },
	{ f: 0.75, x: 57.13, y: 62.84, speed: 214.26 },
	{ f: 1.0,  x: 0.0,   y: 0.0,   speed: 100.0  },
]
for (var i = 0; i < array_length(get_X_test); i++) {
	var t = get_X_test[i];
	assert_almost(path_get_x(p, t.f), t.x, "path_get_x(" + string(t.f) + ")");
	assert_almost(path_get_y(p, t.f), t.y, "path_get_y(" + string(t.f) + ")");
	assert_almost(path_get_speed(p, t.f), t.speed, "path_get_speed(" + string(t.f) + ")");
}

// change_point test
points[0] = { x: 20, y: 30, speed: 40 };
path_change_point(p, 0, points[0].x, points[0].y, points[0].speed);
validate_path_points(p, points, "change_point");

// insert_point test
array_insert(points, 1, {x: 30, y: 60, speed: 80});
path_insert_point(p, 1, points[1].x, points[1].y, points[1].speed);
validate_path_points(p, points, "insert_point");

// delete_point test
array_delete(points, 1, 1);
path_delete_point(p, 1);
validate_path_points(p, points, "delete_point");

// append test
new_path = path_add();
new_points = [
    { x: 200, y: 210, speed: 400 },
    { x: 300, y: 310, speed: 500 },
    { x: 400, y: 440, speed: 600 }
];
populate_path_with_points(new_path, new_points, "populate_path_with_points");
points = array_concat(points, new_points);
path_append(p, new_path);
validate_path_points(p, points, "append");

// assign test
new_points = points;
path_assign(new_path, p);
validate_path_points(new_path, new_points, "assign");

// duplicate test
duplicate_points = new_points;
duplicate_path = path_duplicate(new_path);
validate_path_points(duplicate_path, duplicate_points, "duplicate");

// flip test
points_flip(points);
path_flip(p);
validate_path_points(p, points, "flip");

// mirror test
points_mirror(points);
path_mirror(p);
validate_path_points(p, points, "mirror");

// reverse test
points_reverse(points);
path_reverse(p)
validate_path_points(p, points, "reverse");

// rotate test
points_rotate(points, degtorad(40));
path_rotate(p, 40);
validate_path_points(p, points, "rotate");

// rescale test
points_rescale(points, 1.4, 2.6);
path_rescale(p, 1.4, 2.6);
validate_path_points(p, points, "rescale");

// shift test
points_shift(points, 2.6, 8.4);
path_shift(p, 2.6, 8.4);
validate_path_points(p, points, "shift");

// clear_points test
points = []
path_clear_points(p);
validate_path_points(p, points, "clear_points");

// delete test
path_delete(p);
assert_equals(path_exists(p), false, "path_exists after delete");

show_debug_message("Passed " + string(global.asserts_passed) + " tests out of " + string(global.asserts_total));

game_end();