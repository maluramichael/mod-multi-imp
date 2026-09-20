# mod-multi-imp

Lets a warlock have more than one imp (Kanboard ticket #778).

- The real imp stays the only "Pet": pet bar, DB persistence, talents.
- Each cast of **Summon Imp** (688) also spawns `MultiImp.ExtraCount` extra imps as
  guardians (SummonProperties 61, entry 416).
- Extras **inherit the real imp's state**: react state (aggressive / defensive / passive),
  follow / stay, and attack target. Firebolt is cast by the module (guardians get no
  PetAI from the core).
- Re-casting replaces the extras (no stacking). They are topped up after combat and
  vanish with the real imp. Summoning another demon ends the effect.
- Extras are not on the pet bar and not saved in the DB.

Config: `conf/mod_multi_imp.conf.dist` (`MultiImp.*`). No SQL.

Build note: this module is compiled automatically because it lives in `modules/`.
Function `Addmod_multi_impScripts()` in `src/MultiImp_loader.cpp` is required by the
generated module loader.
