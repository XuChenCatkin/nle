/* Copyright (c) Facebook, Inc. and its affiliates. */
#include <atomic>
#include <cstdio>
#include <memory>

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>

// "digit" is declared in both Python's longintrepr.h and NetHack's extern.h.
#define digit nethack_digit

extern "C" {
#include "hack.h"
#include "permonst.h"
#include "pm.h" // File generated during NetHack compilation.
#include "rm.h"
}

extern "C" {
#include "nledl.h"
}

// Undef name clashes between NetHack and Python.
#undef yn
#undef min
#undef max

#ifdef NLE_USE_TILES
extern short glyph2tile[]; /* in tile.c (made from tilemap.c) */

/* Copy from dungeon.c. Necessary to add tile.c.
   Can't add dungeon.c itself as it pulls in too much. */

/* are you in one of the Hell levels? */
boolean
In_hell(d_level *lev)
{
    return (boolean) (dungeons[lev->dnum].flags.hellish);
}

/* are you in the mines dungeon? */
boolean
In_mines(d_level *lev)
{
    return (boolean) (lev->dnum == mines_dnum);
}

/* are "lev1" and "lev2" actually the same? */
boolean
on_level(d_level *lev1, d_level *lev2)
{
    return (boolean) (lev1->dnum == lev2->dnum
                      && lev1->dlevel == lev2->dlevel);
}
/* End of copy from dungeon.c */
#endif

namespace py = pybind11;
using namespace py::literals;

template <typename T>
T *
checked_conversion(py::handle h, const std::vector<ssize_t> &shape)
{
    if (h.is_none())
        return nullptr;
    if (!py::isinstance<py::array>(h))
        throw std::invalid_argument("Numpy array required");

    py::array array = py::array::ensure(h);
    // We don't use py::array_t<T> (or <T, 0>) above as that still
    // causes conversions to "larger" types.
    if (!array.dtype().is(py::dtype::of<T>()))
        throw std::invalid_argument("Buffer dtype mismatch.");

    py::buffer_info buf = array.request();

    if (buf.ndim != shape.size()) {
        std::ostringstream ss;
        ss << "Array has wrong number of dimensions (expected "
           << shape.size() << ", got " << buf.ndim << ")";
        throw std::invalid_argument(ss.str());
    }
    if (!std::equal(shape.begin(), shape.end(), buf.shape.begin())) {
        std::ostringstream ss;
        ss << "Array has wrong shape (expected [ ";
        for (auto i : shape)
            ss << i << " ";
        ss << "], got [ ";
        for (auto i : buf.shape)
            ss << i << " ";
        ss << "])";
        throw std::invalid_argument(ss.str());
    }
    if (!(array.flags() & py::array::c_style))
        throw std::invalid_argument("Array isn't C contiguous");

    return static_cast<T *>(buf.ptr);
}

class Nethack
{
  public:
    Nethack(std::string dlpath, std::string ttyrec, std::string hackdir,
            std::string nethackoptions, bool spawn_monsters,
            std::string scoreprefix)
        : Nethack(std::move(dlpath), std::move(hackdir),
                  std::move(nethackoptions), spawn_monsters)
    {
        ttyrec_ = std::fopen(ttyrec.c_str(), "a");
        if (!ttyrec_) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, ttyrec.c_str());
            throw py::error_already_set();
        }

        if (ttyrec.size() > sizeof(settings_.scoreprefix) - 1) {
            throw std::length_error("ttyrec filepath too long");
        }

        if (scoreprefix.size() > sizeof(settings_.scoreprefix) - 1) {
            throw std::length_error("scoreprefix too long");
        }
        strncpy(settings_.scoreprefix, scoreprefix.c_str(),
                scoreprefix.length());
        std::size_t found = ttyrec.rfind("/");
        if (found != std::string::npos && found + 1 < ttyrec.length())
            strncpy(settings_.ttyrecname, &ttyrec.c_str()[found + 1],
                    ttyrec.length() - found - 1);

        settings_.initial_seeds.use_init_seeds = false;
        settings_.initial_seeds.use_lgen_seed = false;
    }

    Nethack(std::string dlpath, std::string hackdir,
            std::string nethackoptions, bool spawn_monsters)
        : dlpath_(std::move(dlpath)), obs_{}, settings_{}
    {
        if (hackdir.size() > sizeof(settings_.hackdir) - 1) {
            throw std::length_error("hackdir too long");
        }
        if (nethackoptions.size() > sizeof(settings_.options)) {
            throw std::length_error("nethackoptions too long");
        }

        strncpy(settings_.hackdir, hackdir.c_str(),
                sizeof(settings_.hackdir));
        strncpy(settings_.options, nethackoptions.c_str(),
                sizeof(settings_.options));
        settings_.spawn_monsters = spawn_monsters;
    }

    ~Nethack()
    {
        close();
        if (ttyrec_) {
            fclose(ttyrec_);
        }
    }

    void
    step(int action)
    {
        if (!nle_)
            throw std::runtime_error("step called without reset()");
        if (obs_.done)
            throw std::runtime_error("Called step on finished NetHack");
        obs_.action = action;
        nle_ = nle_step(nle_, &obs_);
    }

    bool
    done()
    {
        return obs_.done;
    }

    void
    reset()
    {
        reset(nullptr);
    }

    void
    reset(std::string ttyrec)
    {
        FILE *f = std::fopen(ttyrec.c_str(), "a");
        if (!f) {
            PyErr_SetFromErrnoWithFilename(PyExc_OSError, ttyrec.c_str());
            throw py::error_already_set();
        }

        std::size_t found = ttyrec.rfind("/");
        if (found != std::string::npos && (found + 1) < ttyrec.length())
            strncpy(settings_.ttyrecname, &ttyrec.c_str()[found + 1],
                    ttyrec.length() - found - 1);

        // Reset environment, then close original FILE. Cannot use freopen
        // as the game may still need to write to the original file but
        // reset() wants to get the new one already.
        reset(f);
        if (ttyrec_) {
            fclose(ttyrec_);
        }
        ttyrec_ = f;
    }

    void
    set_buffers(py::object glyphs, py::object chars, py::object colors,
                py::object specials, py::object blstats, py::object message,
                py::object program_state, py::object internal,
                py::object inv_glyphs, py::object inv_letters,
                py::object inv_oclasses, py::object inv_strs,
                py::object screen_descriptions, py::object tty_chars,
                py::object tty_colors, py::object tty_cursor, py::object misc)
    {
        if (nle_)
            throw std::runtime_error("set_buffers called after reset()");

        std::vector<ssize_t> dungeon{ ROWNO, COLNO - 1 };
        obs_.glyphs = checked_conversion<int16_t>(glyphs, dungeon);
        obs_.chars = checked_conversion<uint8_t>(chars, dungeon);
        obs_.colors = checked_conversion<uint8_t>(colors, dungeon);
        obs_.specials = checked_conversion<uint8_t>(specials, dungeon);
        obs_.blstats =
            checked_conversion<long>(blstats, { NLE_BLSTATS_SIZE });
        obs_.message = checked_conversion<uint8_t>(message, { 256 });
        obs_.program_state = checked_conversion<int>(
            std::move(program_state), { NLE_PROGRAM_STATE_SIZE });
        obs_.internal =
            checked_conversion<int>(internal, { NLE_INTERNAL_SIZE });
        obs_.inv_glyphs =
            checked_conversion<int16_t>(inv_glyphs, { NLE_INVENTORY_SIZE });
        obs_.inv_letters =
            checked_conversion<uint8_t>(inv_letters, { NLE_INVENTORY_SIZE });
        obs_.inv_oclasses =
            checked_conversion<uint8_t>(inv_oclasses, { NLE_INVENTORY_SIZE });
        obs_.inv_strs = checked_conversion<uint8_t>(
            inv_strs, { NLE_INVENTORY_SIZE, NLE_INVENTORY_STR_LENGTH });
        obs_.screen_descriptions = checked_conversion<uint8_t>(
            screen_descriptions,
            { ROWNO, COLNO - 1, NLE_SCREEN_DESCRIPTION_LENGTH });
        obs_.tty_chars = checked_conversion<uint8_t>(
            tty_chars, { NLE_TERM_LI, NLE_TERM_CO });
        obs_.tty_colors = checked_conversion<int8_t>(
            tty_colors, { NLE_TERM_LI, NLE_TERM_CO });
        obs_.tty_cursor = checked_conversion<uint8_t>(tty_cursor, { 2 });
        obs_.misc = checked_conversion<int32_t>(misc, { NLE_MISC_SIZE });

        py_buffers_ = { std::move(glyphs),
                        std::move(chars),
                        std::move(colors),
                        std::move(specials),
                        std::move(blstats),
                        std::move(message),
                        std::move(program_state),
                        std::move(internal),
                        std::move(inv_glyphs),
                        std::move(inv_letters),
                        std::move(inv_oclasses),
                        std::move(inv_strs),
                        std::move(screen_descriptions),
                        std::move(tty_chars),
                        std::move(tty_colors),
                        std::move(tty_cursor),
                        std::move(misc) };
    }

    void
    close()
    {
        if (nle_) {
            nle_end(nle_);
            nle_ = nullptr;
        }
    }

    void
    set_initial_seeds(unsigned long core, unsigned long disp, bool reseed,
                      py::object pyLgen)
    {
        settings_.initial_seeds.seeds[0] = core;
        settings_.initial_seeds.seeds[1] = disp;
        settings_.initial_seeds.reseed = reseed;
        settings_.initial_seeds.use_init_seeds = true;

        /* The level generation seed's optional so may be passed as a Python
           None object, if there isn't any seed. Also, catches other rubbish
           that might be passed in. */
        try {
            settings_.initial_seeds.lgen_seed = pyLgen.cast<unsigned long>();
            settings_.initial_seeds.use_lgen_seed = true;
        } catch (py::cast_error) {
            settings_.initial_seeds.lgen_seed = 0;
            settings_.initial_seeds.use_lgen_seed = false;
        }
    }

    void
    set_seeds(unsigned long core, unsigned long disp, bool reseed,
              py::object pyLgen)
    {
        if (!nle_)
            throw std::runtime_error("set_seed called without reset()");

        unsigned long lgen;
        try {
            lgen = pyLgen.cast<unsigned long>();
        } catch (py::cast_error) {
            /* Is 0 a valid seed number? Does nothing even matter?
               A philosophical question for another day and time. */
            lgen = 0;
        }
        nle_set_seed(nle_, core, disp, reseed, lgen);
    }

    std::tuple<unsigned long, unsigned long, bool, py::object>
    get_seeds()
    {
        if (!nle_)
            throw std::runtime_error("get_seed called without reset()");

        std::tuple<unsigned long, unsigned long, bool, unsigned long, bool>
            result;

        /* NetHack's booleans are not necessarily C++ bools ... */
        char reseed;

        nle_get_seed(nle_, &std::get<0>(result), &std::get<1>(result),
                     &reseed, &std::get<3>(result), &std::get<4>(result));

        /* Package up the seeds as the level generation seed is optional */
        std::tuple<unsigned long, unsigned long, bool, py::object> seeds;
        std::get<0>(seeds) = std::get<0>(result);
        std::get<1>(seeds) = std::get<1>(result);
        std::get<2>(seeds) = reseed;
        /* Only want to return the level generation seed if it's in use */
        if (std::get<4>(result)) {
            std::get<3>(seeds) = py::cast(std::get<3>(result));
        } else {
            std::get<3>(seeds) = py::none();
        }
        return seeds;
    }

    boolean
    in_normal_game()
    {
        return obs_.in_normal_game;
    }

    game_end_types
    how_done()
    {
        return static_cast<game_end_types>(obs_.how_done);
    }

    void
    set_wizkit(std::string wizkit)
    {
        if (wizkit.size() > sizeof(settings_.wizkit)) {
            throw std::length_error("wizkit too long");
        }
        strncpy(settings_.wizkit, wizkit.c_str(), sizeof(settings_.wizkit));
    }

  private:
    void
    reset(FILE *ttyrec)
    {
        py::gil_scoped_release gil;

        if (!ttyrec)
            strncpy(settings_.ttyrecname, "", sizeof(settings_.ttyrecname));

        if (!nle_) {
            nle_ = nle_start(dlpath_.c_str(), &obs_,
                             ttyrec ? ttyrec : ttyrec_, &settings_);
        } else
            nle_reset(nle_, &obs_, ttyrec, &settings_);

        /* Once the seeds have been used, prevent them being reused. */
        settings_.initial_seeds.use_init_seeds = false;
        settings_.initial_seeds.use_lgen_seed = false;

        if (obs_.done)
            throw std::runtime_error("NetHack done right after reset");
    }

    std::string dlpath_;
    nle_obs obs_;
    std::vector<py::object> py_buffers_;
    nledl_ctx *nle_ = nullptr;
    std::FILE *ttyrec_ = nullptr;
    nle_settings settings_;
};

PYBIND11_MODULE(_pynethack, m)
{
    m.doc() = "The NetHack Learning Environment";

    py::class_<Nethack>(m, "Nethack")
        .def(py::init<std::string, std::string, std::string, std::string,
                      bool, std::string>(),
             py::arg("dlpath"), py::arg("ttyrec"), py::arg("hackdir"),
             py::arg("nethackoptions"), py::arg("spawn_monsters") = true,
             py::arg("scoreprefix") = "")
        .def(py::init<std::string, std::string, std::string, bool>(),
             py::arg("dlpath"), py::arg("hackdir"), py::arg("nethackoptions"),
             py::arg("spawn_monsters") = true)
        .def("step", &Nethack::step, py::arg("action"))
        .def("done", &Nethack::done)
        .def("reset", py::overload_cast<>(&Nethack::reset))
        .def("reset", py::overload_cast<std::string>(&Nethack::reset))
        .def("set_buffers", &Nethack::set_buffers,
             py::arg("glyphs") = py::none(), py::arg("chars") = py::none(),
             py::arg("colors") = py::none(), py::arg("specials") = py::none(),
             py::arg("blstats") = py::none(), py::arg("message") = py::none(),
             py::arg("program_state") = py::none(),
             py::arg("internal") = py::none(),
             py::arg("inv_glyphs") = py::none(),
             py::arg("inv_letters") = py::none(),
             py::arg("inv_oclasses") = py::none(),
             py::arg("inv_strs") = py::none(),
             py::arg("screen_descriptions") = py::none(),
             py::arg("tty_chars") = py::none(),
             py::arg("tty_colors") = py::none(),
             py::arg("tty_cursor") = py::none(), py::arg("misc") = py::none())
        .def("close", &Nethack::close)
        .def("set_initial_seeds", &Nethack::set_initial_seeds)
        .def("set_seeds", &Nethack::set_seeds)
        .def("get_seeds", &Nethack::get_seeds)
        .def("in_normal_game", &Nethack::in_normal_game)
        .def("how_done", &Nethack::how_done)
        .def("set_wizkit", &Nethack::set_wizkit);

    py::module mn = m.def_submodule(
        "nethack", "Collection of NetHack constants and functions");

    /* NLE specific constants. */
    mn.attr("NLE_MESSAGE_SIZE") = py::int_(NLE_MESSAGE_SIZE);
    mn.attr("NLE_BLSTATS_SIZE") = py::int_(NLE_BLSTATS_SIZE);
    mn.attr("NLE_PROGRAM_STATE_SIZE") = py::int_(NLE_PROGRAM_STATE_SIZE);
    mn.attr("NLE_INTERNAL_SIZE") = py::int_(NLE_INTERNAL_SIZE);
    mn.attr("NLE_MISC_SIZE") = py::int_(NLE_MISC_SIZE);
    mn.attr("NLE_INVENTORY_SIZE") = py::int_(NLE_INVENTORY_SIZE);
    mn.attr("NLE_INVENTORY_STR_LENGTH") = py::int_(NLE_INVENTORY_STR_LENGTH);
    mn.attr("NLE_SCREEN_DESCRIPTION_LENGTH") =
        py::int_(NLE_SCREEN_DESCRIPTION_LENGTH);

    mn.attr("NLE_BL_X") = py::int_(NLE_BL_X);
    mn.attr("NLE_BL_Y") = py::int_(NLE_BL_Y);
    mn.attr("NLE_BL_STR25") = py::int_(NLE_BL_STR25);
    mn.attr("NLE_BL_STR125") = py::int_(NLE_BL_STR125);
    mn.attr("NLE_BL_DEX") = py::int_(NLE_BL_DEX);
    mn.attr("NLE_BL_CON") = py::int_(NLE_BL_CON);
    mn.attr("NLE_BL_INT") = py::int_(NLE_BL_INT);
    mn.attr("NLE_BL_WIS") = py::int_(NLE_BL_WIS);
    mn.attr("NLE_BL_CHA") = py::int_(NLE_BL_CHA);
    mn.attr("NLE_BL_SCORE") = py::int_(NLE_BL_SCORE);
    mn.attr("NLE_BL_HP") = py::int_(NLE_BL_HP);
    mn.attr("NLE_BL_HPMAX") = py::int_(NLE_BL_HPMAX);
    mn.attr("NLE_BL_DEPTH") = py::int_(NLE_BL_DEPTH);
    mn.attr("NLE_BL_GOLD") = py::int_(NLE_BL_GOLD);
    mn.attr("NLE_BL_ENE") = py::int_(NLE_BL_ENE);
    mn.attr("NLE_BL_ENEMAX") = py::int_(NLE_BL_ENEMAX);
    mn.attr("NLE_BL_AC") = py::int_(NLE_BL_AC);
    mn.attr("NLE_BL_HD") = py::int_(NLE_BL_HD);
    mn.attr("NLE_BL_XP") = py::int_(NLE_BL_XP);
    mn.attr("NLE_BL_EXP") = py::int_(NLE_BL_EXP);
    mn.attr("NLE_BL_TIME") = py::int_(NLE_BL_TIME);
    mn.attr("NLE_BL_HUNGER") = py::int_(NLE_BL_HUNGER);
    mn.attr("NLE_BL_CAP") = py::int_(NLE_BL_CAP);
    mn.attr("NLE_BL_DNUM") = py::int_(NLE_BL_DNUM);
    mn.attr("NLE_BL_DLEVEL") = py::int_(NLE_BL_DLEVEL);
    mn.attr("NLE_BL_CONDITION") = py::int_(NLE_BL_CONDITION);
    mn.attr("NLE_BL_ALIGN") = py::int_(NLE_BL_ALIGN);

    /* NetHack constants. */
    mn.attr("ROWNO") = py::int_(ROWNO);
    mn.attr("COLNO") = py::int_(COLNO);
    mn.attr("NLE_TERM_LI") = py::int_(NLE_TERM_LI);
    mn.attr("NLE_TERM_CO") = py::int_(NLE_TERM_CO);

    mn.attr("NHW_MESSAGE") = py::int_(NHW_MESSAGE);
    mn.attr("NHW_STATUS") = py::int_(NHW_STATUS);
    mn.attr("NHW_MAP") = py::int_(NHW_MAP);
    mn.attr("NHW_MENU") = py::int_(NHW_MENU);
    mn.attr("NHW_TEXT") = py::int_(NHW_TEXT);

    // Cannot include wintty.h as it redefines putc etc.
    // MAXWIN is #defined as 20 there.
    mn.attr("MAXWIN") = py::int_(20);

    mn.attr("NUMMONS") = py::int_(NUMMONS);
    mn.attr("NUM_OBJECTS") = py::int_(NUM_OBJECTS);

    // Glyph array offsets. This is what the glyph_is_* functions
    // are based on, see display.h.
    mn.attr("GLYPH_MON_OFF") = py::int_(GLYPH_MON_OFF);
    mn.attr("GLYPH_PET_OFF") = py::int_(GLYPH_PET_OFF);
    mn.attr("GLYPH_INVIS_OFF") = py::int_(GLYPH_INVIS_OFF);
    mn.attr("GLYPH_DETECT_OFF") = py::int_(GLYPH_DETECT_OFF);
    mn.attr("GLYPH_BODY_OFF") = py::int_(GLYPH_BODY_OFF);
    mn.attr("GLYPH_RIDDEN_OFF") = py::int_(GLYPH_RIDDEN_OFF);
    mn.attr("GLYPH_OBJ_OFF") = py::int_(GLYPH_OBJ_OFF);
    mn.attr("GLYPH_CMAP_OFF") = py::int_(GLYPH_CMAP_OFF);
    mn.attr("GLYPH_EXPLODE_OFF") = py::int_(GLYPH_EXPLODE_OFF);
    mn.attr("GLYPH_ZAP_OFF") = py::int_(GLYPH_ZAP_OFF);
    mn.attr("GLYPH_SWALLOW_OFF") = py::int_(GLYPH_SWALLOW_OFF);
    mn.attr("GLYPH_WARNING_OFF") = py::int_(GLYPH_WARNING_OFF);
    mn.attr("GLYPH_STATUE_OFF") = py::int_(GLYPH_STATUE_OFF);
    mn.attr("MAX_GLYPH") = py::int_(MAX_GLYPH);

    mn.attr("NO_GLYPH") = py::int_(NO_GLYPH);
    mn.attr("GLYPH_INVISIBLE") = py::int_(GLYPH_INVISIBLE);

    mn.attr("MAXEXPCHARS") = py::int_(MAXEXPCHARS);
    mn.attr("MAXPCHARS") = py::int_(static_cast<int>(MAXPCHARS));
    mn.attr("EXPL_MAX") = py::int_(static_cast<int>(EXPL_MAX));
    mn.attr("NUM_ZAP") = py::int_(static_cast<int>(NUM_ZAP));
    mn.attr("WARNCOUNT") = py::int_(static_cast<int>(WARNCOUNT));

    // From objclass.h
    mn.attr("RANDOM_CLASS") = py::int_(static_cast<int>(
        RANDOM_CLASS)); /* used for generating random objects */
    mn.attr("ILLOBJ_CLASS") = py::int_(static_cast<int>(ILLOBJ_CLASS));
    mn.attr("WEAPON_CLASS") = py::int_(static_cast<int>(WEAPON_CLASS));
    mn.attr("ARMOR_CLASS") = py::int_(static_cast<int>(ARMOR_CLASS));
    mn.attr("RING_CLASS") = py::int_(static_cast<int>(RING_CLASS));
    mn.attr("AMULET_CLASS") = py::int_(static_cast<int>(AMULET_CLASS));
    mn.attr("TOOL_CLASS") = py::int_(static_cast<int>(TOOL_CLASS));
    mn.attr("FOOD_CLASS") = py::int_(static_cast<int>(FOOD_CLASS));
    mn.attr("POTION_CLASS") = py::int_(static_cast<int>(POTION_CLASS));
    mn.attr("SCROLL_CLASS") = py::int_(static_cast<int>(SCROLL_CLASS));
    mn.attr("SPBOOK_CLASS") =
        py::int_(static_cast<int>(SPBOOK_CLASS)); /* actually SPELL-book */
    mn.attr("WAND_CLASS") = py::int_(static_cast<int>(WAND_CLASS));
    mn.attr("COIN_CLASS") = py::int_(static_cast<int>(COIN_CLASS));
    mn.attr("GEM_CLASS") = py::int_(static_cast<int>(GEM_CLASS));
    mn.attr("ROCK_CLASS") = py::int_(static_cast<int>(ROCK_CLASS));
    mn.attr("BALL_CLASS") = py::int_(static_cast<int>(BALL_CLASS));
    mn.attr("CHAIN_CLASS") = py::int_(static_cast<int>(CHAIN_CLASS));
    mn.attr("VENOM_CLASS") = py::int_(static_cast<int>(VENOM_CLASS));
    mn.attr("MAXOCLASSES") = py::int_(static_cast<int>(MAXOCLASSES));

    // From monsym.h.
    mn.attr("MAXMCLASSES") = py::int_(static_cast<int>(MAXMCLASSES));

    // From botl.h.
    mn.attr("BL_MASK_STONE") = py::int_(static_cast<int>(BL_MASK_STONE));
    mn.attr("BL_MASK_SLIME") = py::int_(static_cast<int>(BL_MASK_SLIME));
    mn.attr("BL_MASK_STRNGL") = py::int_(static_cast<int>(BL_MASK_STRNGL));
    mn.attr("BL_MASK_FOODPOIS") =
        py::int_(static_cast<int>(BL_MASK_FOODPOIS));
    mn.attr("BL_MASK_TERMILL") = py::int_(static_cast<int>(BL_MASK_TERMILL));
    mn.attr("BL_MASK_BLIND") = py::int_(static_cast<int>(BL_MASK_BLIND));
    mn.attr("BL_MASK_DEAF") = py::int_(static_cast<int>(BL_MASK_DEAF));
    mn.attr("BL_MASK_STUN") = py::int_(static_cast<int>(BL_MASK_STUN));
    mn.attr("BL_MASK_CONF") = py::int_(static_cast<int>(BL_MASK_CONF));
    mn.attr("BL_MASK_HALLU") = py::int_(static_cast<int>(BL_MASK_HALLU));
    mn.attr("BL_MASK_LEV") = py::int_(static_cast<int>(BL_MASK_LEV));
    mn.attr("BL_MASK_FLY") = py::int_(static_cast<int>(BL_MASK_FLY));
    mn.attr("BL_MASK_RIDE") = py::int_(static_cast<int>(BL_MASK_RIDE));
    mn.attr("BL_MASK_BITS") = py::int_(static_cast<int>(BL_MASK_BITS));

    // game_end_types from hack.h (used in end.c)
    py::enum_<game_end_types>(mn, "game_end_types",
                              "This is the way the game ends.")
        .value("DIED", DIED)
        .value("CHOKING", CHOKING)
        .value("POISONING", POISONING)
        .value("STARVING", STARVING)
        .value("DROWNING", DROWNING)
        .value("BURNING", BURNING)
        .value("DISSOLVED", DISSOLVED)
        .value("CRUSHING", CRUSHING)
        .value("STONING", STONING)
        .value("TURNED_SLIME", TURNED_SLIME)
        .value("GENOCIDED", GENOCIDED)
        .value("PANICKED", PANICKED)
        .value("TRICKED", TRICKED)
        .value("QUIT", QUIT)
        .value("ESCAPED", ESCAPED)
        .value("ASCENDED", ASCENDED)
        .export_values();

    // "Special" mapglyph
    mn.attr("MG_CORPSE") = py::int_(MG_CORPSE);
    mn.attr("MG_INVIS") = py::int_(MG_INVIS);
    mn.attr("MG_DETECT") = py::int_(MG_DETECT);
    mn.attr("MG_PET") = py::int_(MG_PET);
    mn.attr("MG_RIDDEN") = py::int_(MG_RIDDEN);
    mn.attr("MG_STATUE") = py::int_(MG_STATUE);
    mn.attr("MG_OBJPILE") =
        py::int_(MG_OBJPILE); /* more than one stack of objects */
    mn.attr("MG_BW_LAVA") = py::int_(MG_BW_LAVA); /* 'black & white lava' */

    // Expose macros as Python functions, with optional vectorization.
    mn.def("glyph_is_monster",
           py::vectorize([](int glyph) { return glyph_is_monster(glyph); }));
    mn.def("glyph_is_normal_monster", py::vectorize([](int glyph) {
               return glyph_is_normal_monster(glyph);
           }));
    mn.def("glyph_is_pet",
           py::vectorize([](int glyph) { return glyph_is_pet(glyph); }));
    mn.def("glyph_is_body",
           py::vectorize([](int glyph) { return glyph_is_body(glyph); }));
    mn.def("glyph_is_statue",
           py::vectorize([](int glyph) { return glyph_is_statue(glyph); }));
    mn.def("glyph_is_ridden_monster", py::vectorize([](int glyph) {
               return glyph_is_ridden_monster(glyph);
           }));
    mn.def("glyph_is_detected_monster", py::vectorize([](int glyph) {
               return glyph_is_detected_monster(glyph);
           }));
    mn.def("glyph_is_invisible", py::vectorize([](int glyph) {
               return glyph_is_invisible(glyph);
           }));
    mn.def("glyph_is_normal_object", py::vectorize([](int glyph) {
               return glyph_is_normal_object(glyph);
           }));
    mn.def("glyph_is_object",
           py::vectorize([](int glyph) { return glyph_is_object(glyph); }));
    mn.def("glyph_is_trap",
           py::vectorize([](int glyph) { return glyph_is_trap(glyph); }));
    mn.def("glyph_is_cmap",
           py::vectorize([](int glyph) { return glyph_is_cmap(glyph); }));
    mn.def("glyph_is_swallow",
           py::vectorize([](int glyph) { return glyph_is_swallow(glyph); }));
    mn.def("glyph_is_warning",
           py::vectorize([](int glyph) { return glyph_is_warning(glyph); }));
    mn.def("glyph_to_char",
           py::vectorize([](int glyph) -> unsigned char {
               // Implement the same logic as mapglyph() but without requiring game state
               // Return the character from showsyms[] array using the same index calculation
               int idx = 0;
               
               if (glyph_is_monster(glyph)) {
                   // Monster: mons[glyph].mlet + SYM_OFF_M (for normal monsters)
                   idx = mons[glyph].mlet + SYM_OFF_M;
               } else if (glyph_is_pet(glyph)) {
                   // Pet: same as monster but different glyph range
                   int offset = glyph - GLYPH_PET_OFF;
                   if (offset >= 0 && offset < NUMMONS) {
                       idx = mons[offset].mlet + SYM_OFF_M;
                   }
               } else if (glyph_is_object(glyph)) {
                   // Object: objects[offset].oc_class + SYM_OFF_O
                   int offset = glyph - GLYPH_OBJ_OFF;
                   if (offset >= 0 && offset < NUM_OBJECTS) {
                       idx = objects[offset].oc_class + SYM_OFF_O;
                       // Special case for boulder
                       if (offset == BOULDER) {
                           idx = SYM_BOULDER + SYM_OFF_X;
                       }
                   }
               } else if (glyph_is_cmap(glyph)) {
                   // Map feature: offset + SYM_OFF_P
                   int offset = glyph - GLYPH_CMAP_OFF;
                   if (offset >= 0 && offset < MAXPCHARS) {
                       idx = offset + SYM_OFF_P;
                   }
               } else if (glyph_is_body(glyph)) {
                   // Corpse: use corpse object symbol
                   idx = objects[CORPSE].oc_class + SYM_OFF_O;
               } else if (glyph_is_statue(glyph)) {
                   // Statue: use monster symbol
                   int offset = glyph - GLYPH_STATUE_OFF;
                   if (offset >= 0 && offset < NUMMONS) {
                       idx = mons[offset].mlet + SYM_OFF_M;
                   }
               } else if (glyph_is_invisible(glyph)) {
                   // Invisible: special symbol
                   idx = SYM_INVISIBLE + SYM_OFF_X;
               } else {
                   // Default case - treat as space
                   return (unsigned char)' ';
               }
               
               // Return the character from showsyms array
               if (idx >= 0 && idx < SYM_MAX) {
                   return (unsigned char)showsyms[idx];
               }
               return (unsigned char)' ';
           }), "Returns the character for a glyph using showsyms lookup like mapglyph().");
    mn.def("glyph_to_color",
           py::vectorize([](int glyph) -> int {
               // Implement color logic similar to mapglyph() 
               int color = NO_COLOR;
               
               if (glyph_is_monster(glyph)) {
                   // Monster color from mons[].mcolor
                   if (glyph < NUMMONS) {
                       color = mons[glyph].mcolor;
                   }
               } else if (glyph_is_pet(glyph)) {
                   // Pet color
                   int offset = glyph - GLYPH_PET_OFF;
                   if (offset >= 0 && offset < NUMMONS) {
                       color = mons[offset].mcolor;
                   }
               } else if (glyph_is_object(glyph)) {
                   // Object color from objects[].oc_color
                   int offset = glyph - GLYPH_OBJ_OFF;
                   if (offset >= 0 && offset < NUM_OBJECTS) {
                       color = objects[offset].oc_color;
                   }
               } else if (glyph_is_cmap(glyph)) {
                   // Map feature color from defsyms[].color
                   int offset = glyph - GLYPH_CMAP_OFF;
                   if (offset >= 0 && offset < MAXPCHARS) {
#ifdef TEXTCOLOR
                       color = defsyms[offset].color;
#else
                       color = NO_COLOR;
#endif
                   }
               } else if (glyph_is_body(glyph)) {
                   // Corpse: use monster color for the corpse
                   int offset = glyph - GLYPH_BODY_OFF;
                   if (offset >= 0 && offset < NUMMONS) {
                       color = mons[offset].mcolor;
                   }
               } else if (glyph_is_statue(glyph)) {
                   // Statue: typically red or object color
                   color = objects[STATUE].oc_color;
               } else if (glyph_is_invisible(glyph)) {
                   // Invisible: no color
                   color = NO_COLOR;
               } else if (glyph_is_trap(glyph)) {
                   // Trap: typically magenta
                   color = CLR_MAGENTA;
               } else {
                   color = CLR_WHITE; // Default color
               }
               
               return color;
           }), "Returns the color for a glyph using the same logic as mapglyph().");

#ifdef NLE_USE_TILES
    mn.attr("glyph2tile") =
        py::memoryview::from_buffer(glyph2tile, /*shape=*/{ MAX_GLYPH },
                                    /*strides=*/{ sizeof(glyph2tile[0]) },
                                    /*readonly=*/true);
#endif

    py::class_<permonst>(mn, "permonst", "The permonst struct.")
        .def(
            "__init__",
            // See https://github.com/pybind/pybind11/issues/2394
            [](py::detail::value_and_holder &v_h, int index) {
                if (index < 0 || index >= NUMMONS)
                    throw std::out_of_range(
                        "Index should be between 0 and NUMMONS ("
                        + std::to_string(NUMMONS) + ") but got "
                        + std::to_string(index));
                v_h.value_ptr() = &mons[index];
                v_h.inst->owned = false;
                v_h.set_holder_constructed(true);
            },
            py::detail::is_new_style_constructor())
        .def_readonly("mname", &permonst::mname)   /* full name */
        .def_readonly("mlet", &permonst::mlet)     /* symbol */
        .def_readonly("mlevel", &permonst::mlevel) /* base monster level */
        .def_readonly("mmove", &permonst::mmove)   /* move speed */
        .def_readonly("ac", &permonst::ac)         /* (base) armor class */
        .def_readonly("mr", &permonst::mr) /* (base) magic resistance */
        // .def_readonly("maligntyp", &permonst::maligntyp) /* basic
        // monster alignment */
        .def_readonly("geno", &permonst::geno) /* creation/geno mask value */
        // .def_readonly("mattk", &permonst::mattk) /* attacks matrix
        // */
        .def_readonly("cwt", &permonst::cwt) /* weight of corpse */
        .def_readonly("cnutrit",
                      &permonst::cnutrit) /* its nutritional value */
        .def_readonly("msound",
                      &permonst::msound)         /* noise it makes (6 bits) */
        .def_readonly("msize", &permonst::msize) /* physical size (3 bits) */
        .def_readonly("mresists", &permonst::mresists) /* resistances */
        .def_readonly("mconveys",
                      &permonst::mconveys)           /* conveyed by eating */
        .def_readonly("mflags1", &permonst::mflags1) /* boolean bitflags */
        .def_readonly("mflags2",
                      &permonst::mflags2) /* more boolean bitflags */
        .def_readonly("mflags3",
                      &permonst::mflags3) /* yet more boolean bitflags */
        .def_readonly("difficulty",
                      &permonst::difficulty) /* toughness (formerly from
                                                makedefs -m) */
#ifdef TEXTCOLOR
        .def_readonly("mcolor", &permonst::mcolor) /* color to use */
#endif
        ;

    py::class_<class_sym>(mn, "class_sym")
        .def_static(
            "from_mlet",
            [](char let) -> const class_sym * {
                if (let < 0 || let >= MAXMCLASSES)
                    throw std::out_of_range(
                        "Argument should be between 0 and MAXMCLASSES ("
                        + std::to_string(MAXMCLASSES) + ") but got "
                        + std::to_string(let));
                return &def_monsyms[let];
            },
            py::return_value_policy::reference)
        .def_static(
            "from_oc_class",
            [](char olet) -> const class_sym * {
                if (olet < 0 || olet >= MAXOCLASSES)
                    throw std::out_of_range(
                        "Argument should be between 0 and MAXOCLASSES ("
                        + std::to_string(MAXOCLASSES) + ") but got "
                        + std::to_string(olet));
                return &def_oc_syms[olet];
            },
            py::return_value_policy::reference)
        .def_readonly("sym", &class_sym::sym)
        .def_readonly("name", &class_sym::name)
        .def_readonly("explain", &class_sym::explain)
        .def("__repr__", [](const class_sym &cs) {
            return "<nethack.class_sym sym='" + std::string(1, cs.sym)
                   + "' explain='" + std::string(cs.explain) + "'>";
        });

    mn.def("glyph_to_mon", py::vectorize([](int glyph) -> int {
               return glyph_to_mon(glyph);
           }));
    mn.def("glyph_to_obj", py::vectorize([](int glyph) -> int {
               return glyph_to_obj(glyph);
           }));
    mn.def("glyph_to_trap", py::vectorize([](int glyph) -> int {
               return glyph_to_trap(glyph);
           }));
    mn.def("glyph_to_cmap", py::vectorize([](int glyph) -> int {
               return glyph_to_cmap(glyph);
           }));
    mn.def("glyph_to_swallow", py::vectorize([](int glyph) -> int {
               return glyph_to_swallow(glyph);
           }));
    mn.def("glyph_to_warning", py::vectorize([](int glyph) -> int {
               return glyph_to_warning(glyph);
           }));

    py::class_<objclass>(
        mn, "objclass",
        "The objclass struct.\n\n"
        "All fields are constant and don't reflect user changes.")
        .def(
            "__init__",
            // See https://github.com/pybind/pybind11/issues/2394
            [](py::detail::value_and_holder &v_h, int i) {
                if (i < 0 || i >= NUM_OBJECTS)
                    throw std::out_of_range(
                        "Index should be between 0 and NUM_OBJECTS ("
                        + std::to_string(NUM_OBJECTS) + ") but got "
                        + std::to_string(i));

                /* Initialize. Cannot depend on o_init.c as it pulls
                 * in all kinds of other code. Instead, do what
                 * makedefs.c does at set it here.
                 * Alternative: Get the pointer from the game itself?
                 * Dangerous!
                 */
                objects[i].oc_name_idx = objects[i].oc_descr_idx = i;

                v_h.value_ptr() = &objects[i];
                v_h.inst->owned = false;
                v_h.set_holder_constructed(true);
            },
            py::detail::is_new_style_constructor())
        .def_readonly("oc_name_idx",
                      &objclass::oc_name_idx) /* index of actual name */
        .def_readonly(
            "oc_descr_idx",
            &objclass::oc_descr_idx) /* description when name unknown */
        .def_readonly(
            "oc_oprop",
            &objclass::oc_oprop) /* property (invis, &c.) conveyed */
        .def_readonly(
            "oc_class",
            &objclass::oc_class) /* object class (enum obj_class_types) */
        .def_readonly(
            "oc_delay",
            &objclass::oc_delay) /* delay when using such an object */
        .def_readonly("oc_color",
                      &objclass::oc_color) /* color of the object */

        .def_readonly("oc_prob",
                      &objclass::oc_prob) /* probability, used in mkobj() */
        .def_readonly("oc_weight",
                      &objclass::oc_weight) /* encumbrance (1 cn = 0.1 lb.) */
        .def_readonly("oc_cost", &objclass::oc_cost) /* base cost in shops */
        /* And much more, see objclass.h. */;

    mn.def("OBJ_NAME", [](const objclass &obj) { return OBJ_NAME(obj); });
    mn.def("OBJ_DESCR", [](const objclass &obj) { return OBJ_DESCR(obj); });

    py::class_<objdescr>(mn, "objdescr")
        .def_static(
            "from_idx",
            [](int idx) -> const objdescr * {
                if (idx < 0 || idx >= NUM_OBJECTS)
                    throw std::out_of_range(
                        "Argument should be between 0 and NUM_OBJECTS ("
                        + std::to_string(NUM_OBJECTS) + ") but got "
                        + std::to_string(idx));
                return &obj_descr[idx];
            },
            py::return_value_policy::reference)
        .def_readonly("oc_name", &objdescr::oc_name)
        .def_readonly("oc_descr", &objdescr::oc_descr)
        .def("__repr__", [](const objdescr &od) {
            // clang-format doesn't like the _s UDL.
            // clang-format off
            return "<nethack.objdescr oc_name={!r} oc_descr={!r}>"_s
                // clang-format on
                .format(od.oc_name ? py::str(od.oc_name)
                                   : py::object(py::none()),
                        od.oc_descr ? py::str(od.oc_descr)
                                    : py::object(py::none()));
        });

    py::class_<symdef>(mn, "symdef")
        .def_static(
            "from_idx",
            [](int idx) -> const symdef * {
                if (idx < 0 || idx >= MAXPCHARS)
                    throw std::out_of_range(
                        "Argument should be between 0 and MAXPCHARS ("
                        + std::to_string(MAXPCHARS) + ") but got "
                        + std::to_string(idx));
                return &defsyms[idx];
            },
            py::return_value_policy::reference)
        .def_readonly("sym", &symdef::sym)
        .def_readonly("explanation", &symdef::explanation)
#ifdef TEXTCOLOR
        .def_readonly("color", &symdef::color)
#endif
        .def("__repr__", [](const symdef &sd) {
            // clang-format doesn't like the _s UDL.
            // clang-format off
            return "<nethack.symdef sym={!r} explanation={!r}>"_s
                // clang-format on
                .format(std::string(1, sd.sym), sd.explanation
                                                    ? py::str(sd.explanation)
                                                    : py::object(py::none()));
        });
}
