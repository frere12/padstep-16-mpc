# PadStep 16 pour MPC (v0.6.0)

Séquenceur pas à pas monophonique de 16 pas, en plugin VST2 natif pour MPC OS standalone. C'est le portage de notre
séquenceur HTML, sous un autre nom.

Il n'a pas de moteur audio. Les notes sortent par un port MIDI propre au plugin, **« PadStep 16 / MIDI Out »**, sur le canal
choisi, et vont jouer n'importe quelle piste de la MPC. Les **16 pads de la MPC** activent ou coupent les 16 pas, comme les pads
du séquenceur d'origine.

Il est construit avec la chaîne Open MPC de sd88me, sur le même moteur que l'essai Tenori MPC : synchronisation par la position
du morceau, port MIDI et sauvegarde dans le projet.

## Fichiers

| | |
|---|---|
| `PadStep-16-0.6.0-mpc-armv7.zip` | MPC One / Live / Live II / X / Key 61 (Gen1) |
| `PadStep-16-0.6.0-mpc-aarch64.zip` | MPC One G2, Live III, Key G2 (Gen2) |

## Installation et premier test

1. Télécharger le zip de sa MPC dans les *releases* du dépôt et suivre le `INSTALL.md` qu'il contient (installation par
   script, ou copie du dossier à la main).
2. Créer une piste plugin **PadStep 16**. Dans **Preferences → MIDI**, activer **Track** pour le port **PadStep 16**.
3. Créer une piste avec un son, entrée MIDI = port PadStep 16, canal 1, monitoring actif.
4. Lancer **Play** : un motif de démonstration de 16 notes doit jouer, calé sur le tempo de la MPC.
5. Sélectionner la piste PadStep 16 et frapper les pads : chaque pad coupe ou rallume son pas. Par défaut, le pad 1 envoie la
   note 24, comme sur ta MPC One. Si les pads ne tombent pas juste, page SETUP : active **LEARN**, puis frappe les pads 1 à 16
   dans l'ordre. LEARN s'éteint tout seul après le pad 16. L'affectation est enregistrée avec le projet.
   **LAST PAD** (page SETUP) affiche la note reçue et le pad correspondant (« note 26 = pad 3 »).

## Apparence

Elle reprend les couleurs du séquenceur HTML :
- panneau gris clair, encodeurs noirs à trait blanc et arc bleu ;
- pas actifs en bleu, slots enregistrés en ambre, écrans noirs à texte bleu ;
- touches grises, voyants rouges pour SHIFT, CHAN, STORE, RECALL et LEARN.

Les inscriptions gravées sont en Barlow Condensed, en 20 à 24 px. Les valeurs, que la MPC affiche elle-même, sont en 24 à 26 px.
Le skin est dessiné par le moteur « navigateur » de la chaîne Open MPC (`"art": "html"`), à partir de `skin/skin.css` et des
images SVG de `skin/`. La police Barlow Condensed est sous licence OFL : `skin/fonts/OFL-Barlow-Condensed.txt`.

## Pages

**SEQ**
- Rangée de 16 cases tactiles : les pas. Une case allumée est un pas actif, et chaque case affiche sa note. Une fine rangée
  au-dessus montre le pas joué.
- 16 potentiomètres de hauteur, qu'on règle au doigt ou par les Q-Links. Une colonne de 4 à la fois ; le bouton Q-Link passe à la
  colonne suivante.
- Les notes sont calées sur la gamme choisie. Tourner vers le haut va à la note suivante de la gamme, vers le bas à la précédente.
- La ligne **SEQUENCE** affiche gamme, mode, taille de pas, canal et transposition, ou la consigne en cours après SHIFT, CHAN,
  STORE ou RECALL.

**STEP+**
- **TIE** : une case par pas, tap pour lier ou délier. Un pas lié tient sa note jusqu'au pas suivant. Si le pas suivant a la
  même note, elle continue sans être rejouée ; sinon, il s'enchaîne en legato.
- **SLIDE** (cases orange) : glissé vers le pas suivant, façon 303.
  - La note suivante démarre *avant* que la note du pas glissé ne soit relâchée. Un synthé mono réglé en legato avec du
    portamento (ou « glide » en mode legato/fingered) glisse alors d'une note à l'autre, sans redéclencher l'enveloppe.
  - Même note au pas suivant : elle continue, sans être rejouée. Silence au pas suivant : la note s'arrête là.
  - Option **SLIDE CC65** (page SETUP) : envoie CC65 = 127 (portamento On) quand un pas glissé joue, puis CC65 = 0 juste après
    le départ de la note suivante. Pour les synthés qui glissent sur toutes les notes dès que le portamento est actif.
  - Un pas en ratchet ignore SLIDE.
- **RATCHET** : chaque tap fait passer la case de x1 à x2, x3, x4, puis revient à x1. Le pas est rejoué 2, 3 ou 4 fois dans
  sa durée, avec la même note et la même vélocité ; chaque coup reçoit sa part du gate. Un pas en ratchet ignore TIE et SLIDE.
- **CHANCE** : 16 potentiomètres, de 0 à 100 %, aussi sur les Q-Links (sous-page CHANCE). C'est la probabilité que le pas
  joue à chaque passage. S'il ne joue pas, c'est un silence.

**STEP ENTRY** (en bas de la page SEQ, à côté de l'afficheur) : saisie des notes au clavier.
- Allume ENTRY. Chaque note jouée sur la piste PadStep (clavier MIDI, ou pads de la MPC) devient la note du pas suivant :
  hauteur et vélocité, pas allumé. Le curseur avance tout seul et revient au début après la longueur de la séquence.
- **REST** met un silence et avance d'un pas, **BACK** recule d'un pas.
- Toucher une case de pas sur la page SEQ y place le curseur.
- Transport arrêté, le curseur s'affiche sur la rangée de LED rouges, et chaque note saisie s'entend sur la piste cible.
- Une seule pression sur UNDO annule toute la saisie.
- Pendant la saisie, les pads servent de clavier : ils n'activent plus les pas.

**VELOCITY**
- 16 curseurs de vélocité, aussi sur les Q-Links (sous-page VELOCITY).
- **MASTER**, réglable au doigt ou par les Q-Links de la sous-page MASTER :
  - **LEVEL** agit sur les 16 vélocités d'un coup.
  - En mode **SCALE**, il les multiplie en gardant leurs proportions : passer de 100 à 50 les divise toutes par deux, revenir
    à 100 les rétablit exactement. Après un réglage à la main d'un curseur, LEVEL repart de ces nouvelles valeurs.
  - En mode **SET**, LEVEL met les 16 à sa propre valeur.
- **SHAPES** : SAW (descendante), RAMP (montante), SINE, TRI et RANDOM. Chaque forme couvre la longueur de la séquence,
  entre 30 et 127, et se nuance ensuite avec LEVEL. UNDO l'annule.

**TOOLS**
- **SHIFT**, **CHAN**, **STORE**, **RECALL** : comme sur le séquenceur HTML, le prochain pad choisit la fonction.
  - Avec **SHIFT** : pads 1 à 7 = gamme, 9 à 12 = mode FWD, REV, ALT ou RAND, 13 à 16 = taille de pas 1/4 à 1/32.
  - Avec **CHAN** : le pad choisit le canal MIDI.
  - Avec **STORE** ou **RECALL** : le pad choisit le slot.
- **16 slots** en cases : une case allumée contient une séquence, le slot chargé s'affiche `[n]`. Taper une case recharge le
  slot ; en mode STORE, elle l'enregistre.
- **RANDOM**, **MUTATE**, **SHIFT L** et **SHIFT R** (rotation), **REVERSE**, **CLEAR**, **UNDO** (8 niveaux).
- **CHAIN** (enchaînement de séquences, jusqu'à 32 éléments) :
  - **EDIT** allumé : chaque slot touché, ou chaque pad frappé, s'ajoute à la fin de la chaîne. Éteins EDIT quand c'est fini.
    Un même slot peut revenir plusieurs fois, par exemple 1 1 2 1 3.
  - **BACK** retire le dernier élément, **CLEAR** vide la chaîne.
  - **PLAY** allumé : la chaîne joue. Chaque élément joue une passe complète de son slot, avec sa propre longueur, taille
    de pas, mode, gamme, swing et gate (en ALT, l'aller-retour complet), puis passe au suivant et boucle à la fin.
  - L'afficheur montre la chaîne et l'élément en cours : `2/5 1 [2] 1 3 4`.
  - La chaîne est calée sur la position du morceau : elle démarre au début de la mesure 1. Si tu lances la lecture au milieu
    du morceau ou qu'une boucle de la MPC revient en arrière, elle reprend exactement là où elle en serait en jouant d'une
    traite. La transposition en cours est conservée d'un élément à l'autre.
  - Pendant la lecture d'une chaîne, les modifications portent sur la copie du slot qui joue. Fais STORE pour les garder,
    sinon elles disparaissent au passage à l'élément suivant.
  - La chaîne est enregistrée avec le projet.

**SETUP**
- Mode, gamme (Chromatic, Major, Minor, Dorian, Mixolydian, Harm Min, Blues), taille de pas, legato (Off, On, Reset), longueur
  1 à 16, swing 50 à 75 %, gate 50 à 99 %.
- **RANDOM OCTAVES** (LOW, HIGH) : plage d'octaves de RANDOM.
- Canal MIDI, transposition (-24 à +24).
- **PADS** : STEPS pour activer les pas, TRANSP pour transposer (C4 = 0, comme l'entrée MIDI du HTML).
- **PAD BASE** : note du pad 1, les 15 suivantes en chromatique. **LEARN** : affectation pad par pad. **LAST PAD** : dernière
  note reçue.
- **SLIDE CC65** : voir SLIDE, page STEP+.

Le tempo vient de la MPC. Tout, y compris les 16 slots, est enregistré avec le projet.

## Écarts par rapport au séquenceur HTML

- **Retirés** : le synthé intégré, le mode contrôleur (CNTRL et CC des potentiomètres), l'horloge MIDI externe, MIDI Learn, les
  exports .json et .mid. La MPC fait déjà tout cela elle-même ; pour un fichier MIDI, il suffit d'enregistrer la piste.
- **Swing** : il retarde les pas impairs de la valeur standard (75 % = croche pointée-double). Le HTML retardait de moitié moins.
- **Legato Reset** : traité comme On, comme dans le HTML.
- **Lumières des pads** : pas de retour visuel sur les pads de la MPC. À ma connaissance, un plugin ne peut pas les piloter.
  L'état des pas se lit sur l'écran.

## Vérifié, et pas vérifié

Vérifié sur PC :
- `tools/test_port.sh` (ASan/UBSan) : PASSED.
- `test/engine_test.c` : PASSED.
  - 50 boucles avec jitter : nombre exact de pas, aucun doublon, en phase.
  - Swing 66 % et 75 % avec boucles et changements de tempo brusques.
  - Legato sans note bloquée.
  - Pads, y compris SHIFT, CHAN et transposition.
  - Cases tactiles : distinction entre un toucher et le renvoi de la MPC.
  - Potentiomètres calés sur la gamme.
  - Slots, UNDO, RANDOM qui reste dans la gamme.
  - Sauvegarde et rechargement de l'état.
  - Ratchet x3 : nombre et position des coups. Tie vers la même note ou une autre. Chance 0 % et 50 % (sur 400 passages).
  - Saisie au clavier, REST, BACK, curseur, UNDO. Anciennes sauvegardes (v0.4, v0.5) relues sans perte.
  - Slide : la note suivante part avant le relâchement de la précédente, même note tenue, arrêt sur un silence, ordre des
    CC65 (127 avant la note, 0 après la suivante).
  - Test de charge avec ratchets, ties, slides et chance partout, sur boucles avec jitter et swing : jamais plus de deux notes à la
    fois (uniquement pendant un slide), aucune note bloquée.
  - Chaîne : ordre de lecture avec des slots de longueurs et de tailles de pas différentes, démarrage au milieu du morceau,
    boucles, jitter et swing sans doublon, sauvegarde.

Essayé sur MPC One G2 (MPC OS 3.10.1). Les binaires ARM sont compilés avec `zig cc`, avec des symboles glibc ≤ 2.17.

## Recompiler (Linux Mint)

```bash
git clone https://github.com/sd88me/mpc-vst-plugins ~/mpc-vst-plugins
cd padstep-16-mpc
~/mpc-vst-plugins/tools/test_port.sh vst.json        # test PC
~/mpc-vst-plugins/tools/build_port.sh vst.json all   # chaîne officielle (Docker ; construit aussi l'image mpc-vst-html-art du skin)
# tests du moteur :
gcc -DMIDIPORT_TEST -I ~/mpc-vst-plugins/wrapper -Isrc test/engine_test.c src/padstep_engine.c -lm -ldl -o engine_test && ./engine_test
```

Le nom et le fabricant (`Benoit`) se changent dans `vst.json` (`name`, `vendor`). Change aussi `uid` si une autre version doit
coexister.
