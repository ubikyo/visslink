# Bouclage ECS à la demande (v0.5, expérimental)

> **Statut : NON prêt pour la production.** Le code est implémenté, testé en
> simulation et **désactivé par défaut** (`enable_schedule_writes: false`,
> `validated_days: []`). **Aucune écriture de programmation n'a jamais été
> faite par Optolink sur la Vitotronic 300 KW3.** L'essai manuel du
> 15 novembre 2026, fait depuis le panneau, montre que la régulation tient
> compte d'une plage déjà commencée. Il ne valide pas l'écriture Optolink
> d'un bloc de 8 octets. La récupération des horaires n'est garantie qu'à
> deux conditions : la KW3 accepte l'écriture de blocs, et le bloc relu est
> exactement celui qui a été écrit. Ni l'une ni l'autre n'est établie (§ 7).

## 1. Architecture

```text
Home Assistant ── bouton / durée ──► VissLinkBoost (components/visslink_boost)
                                          │  colle ESPHome : exécute les actions,
                                          │  flash ESP8266, entités HA
                                          ▼
                                   BoostSequencer (circulation_boost.h, pur, testé sur PC)
                                          │  actions : LIRE, ÉCRIRE, SAUVEGARDER, EFFACER
                                          ▼
VissLinkComponent ── lecture ponctuelle prioritaire (0x088E, 0x2200..0x2230) ─┐
                 └── écriture de bloc via WriteArbiter (BOOST_OWNER) ──────────┤
                                                                               ▼
                                                        KwProtocol (un seul moteur, 0x05)
```

Principe : la plage temporaire est ajoutée au **programme** de la régulation,
qui reste seule maîtresse du circulateur. `0x0846` n'est jamais écrit. Il
reste lu pour savoir si le circulateur tourne réellement. Une plage
programmée n'est pas un circulateur en marche.

### Machine à états

```text
IDLE ─start─► READ_CLOCK ─► READ_BLOCK ─► BACKUP_PENDING ─► WRITE_PENDING ─► VERIFY_PENDING
  ▲              │ refus        │ refus /       │ échec           │ non émise      │
  │              ▼              ▼ déjà actif    ▼ (rien écrit)    ▼ (rien écrit)   │
  └──────────── IDLE ◄───────── IDLE ◄──────── IDLE ◄── CLEAR ◄──┘                │
                                                                                  ▼
   = temporaire ─► ACTIVE ─(durée écoulée depuis la confirmation, ou bouton)─► RESTORE_READ
   = origine ────► CLEAR_PENDING ─► IDLE (« refusé par la régulation »)               │
   autre / illisible ─► RECOVERY_REQUIRED                                              │
                                                                                       ▼
   RESTORE_READ : = temporaire ─► RESTORE_WRITE ─► RESTORE_VERIFY ─(= origine)─► CLEAR ─► IDLE
                  = origine ────► CLEAR ─► IDLE
                  autre ────────► RECOVERY_REQUIRED (modifié depuis le panneau : jamais écrasé)
   liaison (relecture impossible, trame non émise) ─► ACTIVE, nouvelle relecture dans 30 s,
                  pendant 15 min après la fin de la durée ; au-delà ─► RECOVERY_REQUIRED
   restauration non appliquée ─► nouvelle relecture dans 2 min ; au plus 3 écritures ─► RECOVERY_REQUIRED

   RECOVERY_REQUIRED ─bouton « restaurer »─► RECOVERY_READ ─(= temporaire)─► RECOVERY_WRITE ─► RECOVERY_VERIFY
                     ─bouton « oublier »──► CLEAR (aucune écriture)
```

## 2. Fichiers

| Fichier | Changement |
|---|---|
| `components/visslink/schedule.h` | `ScheduleSlots`, `decode_schedule_slots`, `encode_schedule_slots`, `encode_schedule_time` : codage par **emplacement**, aller-retour exact |
| `components/visslink/vito_clock.h` (nouveau) | décodage et validation de l'horloge `0x088E` |
| `components/visslink/circulation_boost.h` (nouveau) | `plan_boost()`, `BoostRecord`, `BoostSequencer`, liste blanche des blocs |
| `components/visslink/kw_protocol.h` | écriture de **1 ou 8** octets (aucune autre longueur) |
| `components/visslink/write_guard.h` | demande de bloc dans l'arbitre (`submit_block`, `BOOST_OWNER`) |
| `components/visslink/visslink.h/.cpp` | `enable_schedule_writes` ; lecture ponctuelle prioritaire ; écriture de bloc revérifiée avant émission ; `text_sensor` `type: vito_clock` |
| `components/visslink/__init__.py`, `text_sensor.py` | option `enable_schedule_writes` (exige `enable_writes`) ; `type: vito_clock` |
| `components/visslink_boost/` (nouveau) | colle ESPHome : bouton, durée, états, flash |
| `examples/iot-viessmann.yaml` | v0.5 : horloge en lecture, bouclage déclaré **inactif** |
| `tests/cpp/test_boost.cpp` (nouveau), `test_kw_write.cpp`, `test_simulation.cpp`, `sim.h` | tests unitaires et simulations |
| `tests/test_esphome_config.py`, `tests/fixtures/iot-viessmann-v0.4.1.yaml`, `Makefile` | validation YAML, compatibilité v0.4.1 |

Les chemins A1, M2 et consignes ne sont pas modifiés. Ils ne font que
partager l'arbitre (une écriture à la fois).

## 3. Registres

| Registre | Usage | Accès VissLink | État de la validation |
|---|---|---|---|
| `0x0846` | état du circulateur de bouclage | **lecture seule** | `00` → `01` → `00` observé le 15/11 (diagnostic) |
| `0x2200 + 8 × jour` | programmation du bouclage (lundi = 0) | lecture ; écriture **expérimentale** des 7 blocs seulement | lecture : 7 blocs décodés ; **correspondance bloc ↔ jour à valider jour par jour** (`validated_days`) ; écriture : **jamais testée** |
| `0x088E` | horloge de la Vitotronic | **lecture seule** | à comparer à l'afficheur (phase 1) |

### Format des blocs (confirmé : vcontrold `setCycleTime`, décodage VissLink depuis la v0.2.1)

- 8 octets = 4 plages (début, fin) ; `FF FF` = plage libre.
- Octet horaire = `heure × 8 + minutes / 10` ; dizaines de minutes 0 à 5 ;
  `0xC0` = 24:00 (admis en fin, jamais observé ici : **non utilisé**).
- Chaque octet valide a un seul codage : **décoder puis réencoder redonne les
  8 mêmes octets**, pour tout bloc valide, y compris avec des emplacements
  libres au milieu (vérifié sur 100 000 blocs aléatoires).
- Tri et chevauchement : non documentés. Le panneau produit des plages
  triées et compactes. VissLink ne modifie que les blocs déjà sous cette
  forme et refuse les autres (`UNSUPPORTED_LAYOUT`).
- Plage traversant minuit : non documentée. VissLink n'en crée jamais (fin
  ≤ 23:50).
- KW2 / KW3 : même format lu sur la KW3. L'écriture n'est documentée que
  pour la KW2 (vcontrold `setTimerZirku*`).

### Horloge `0x088E` (vcontrold, unité `TI`)

8 octets BCD : siècle, année, mois, jour, jour de la semaine, heure, minute,
seconde. Jour de la semaine : vcontrold calcule `BCD % 7` (1 = lundi …
7 ou 0 = dimanche). Cette convention **n'est pas vérifiée sur la KW3** :
VissLink recoupe le jour lu avec celui calculé à partir de la date et
refuse en cas de désaccord.

## 4. Calcul de la plage

`plan_boost(heure Vitotronic, bloc du jour, durée)` :

- début = heure arrondie **vers le bas** à la dizaine ;
- fin = heure + durée + **2 min de marge**, arrondie **vers le haut** à la
  dizaine. La marge couvre le délai entre la lecture de l'horloge et la
  prise en compte de l'écriture : la plage **restante est toujours ≥ durée
  demandée** (propriété vérifiée pour toutes les heures de la journée) ;
- plage existante contenant déjà l'heure → `ALREADY_ACTIVE`, rien n'est
  écrit, les minutes restantes sont affichées ;
- refus sans écriture : chevauchement, 4 plages déjà utilisées (aucune
  plage existante n'est jamais écrasée), fin après 23:50, bloc invalide ou
  non canonique, durée hors 10/20/30 ;
- insertion **triée** : les plages existantes gardent leurs octets, seules
  celles qui suivent avancent d'un emplacement.

| Heure (durée 10 min) | Plage ajoutée | Restant garanti |
|---|---|---|
| 14:00:00 | 14:00-14:20 | 20 min |
| 14:01 | 14:00-14:20 | 19 min |
| 14:09 | 14:00-14:30 | 21 min |
| 14:10 | 14:10-14:30 | 20 min |
| 14:59 | 14:50-15:20 | 21 min |
| 23:38:00 | 23:30-23:50 | 12 min |
| 23:50, 23:59 | refus (minuit) | — |

**Comparaison avec l'essai du 15 novembre** (programme
`06:10–08:30, 21:40–21:50`, demande vers 14:03) : VissLink calculerait
`31 43 70 72 AC AD FF FF` (06:10-08:30, **14:00-14:20**, 21:40-21:50). La
relecture après l'essai sur le panneau donnait `06:10–08:30, 14:00–14:10` :
**la plage 21:40–21:50 a disparu**. Il faut vérifier sur le panneau si elle
a été remplacée lors de l'essai, et la rétablir au besoin. Le 15/11/2026
était un **dimanche** : le bloc modifié devait être `0x2230`. Si c'est bien
celui-là qui a changé, la correspondance est confirmée pour le dimanche.

## 5. Sauvegarde et restauration

### Ordre imposé

1. Lecture de l'horloge `0x088E`, validation, recoupement avec Home
   Assistant (même jour, ±5 min), jour présent dans `validated_days`.
2. Lecture **fraîche** du bloc du jour, validation, calcul du bloc temporaire.
3. **Sauvegarde persistante** : 8 octets d'origine, 8 octets temporaires,
   jour, adresse, plage, numéro de séquence et somme de contrôle (FNV-1a).
   Elle est écrite en **flash** (préférence ESPHome `in_flash`), puis
   `sync()` force l'écriture immédiate du secteur, et la préférence est
   relue et comparée.
4. Seulement si la sauvegarde est confirmée : **une** écriture du bloc
   temporaire (refusée si l'heure lue a plus de 60 s).
5. Relecture et comparaison **exacte** des 8 octets :
   - temporaire → plage active ;
   - origine → « refusé par la régulation », sauvegarde effacée ;
   - autre ou illisible → **récupération requise**, toute nouvelle
     opération bloquée, aucune écriture.
6. **Durée demandée écoulée depuis la confirmation** (v0.5.2, § 5 bis) :
   relecture du bloc. Restauration **seulement si** le bloc est exactement
   le temporaire, puis relecture : l'effacement de la sauvegarde n'a lieu
   qu'**après** la preuve de l'égalité avec l'origine.

Un accusé `00` n'est jamais considéré comme une preuve : le journal l'indique
explicitement et seule la relecture décide.

### Récupération (après coupure, état indéterminé, conflit)

- Au démarrage, une sauvegarde valide en flash signifie « opération
  incomplète » : alerte **« Restauration nécessaire »**, état détaillé avec
  les octets d'origine et temporaires, tout nouveau déclenchement bloqué,
  **aucune écriture automatique**.
- Bouton « Restaurer la programmation » : relecture, puis comparaison.
  Bloc = temporaire : écriture de l'origine, puis relecture. Bloc =
  origine : sauvegarde effacée, rien d'écrit. Autre : **conflit**, rien
  n'est écrit.
- Bouton « Oublier la sauvegarde » : efface la sauvegarde **sans rien
  écrire**, après correction manuelle depuis le panneau.
- Les octets d'origine figurent dans l'entité d'état (historique Home
  Assistant) et dans les journaux : ils restent disponibles même si l'ESP
  ne fonctionne plus.

### Usure

| Mémoire | Par opération | Limite appliquée | Commentaire |
|---|---|---|---|
| Flash ESP8266 (un secteur, effacé à chaque `sync`) | 2 effacements (sauvegarde, effacement) | 8 opérations/jour, 15 min entre deux | ≤ 16 effacements/jour, soit ~5 800/an ; endurance typique 10 000 à 100 000 cycles par secteur : quelques années au pire à ce rythme maximal |
| Mémoire de la régulation | 2 écritures de 8 octets | idem | endurance inconnue ; usage ponctuel recommandé |

### Actualisation de l'affichage (v0.5.1)

Après une modification **confirmée par la relecture de vérification**, la
programmation publiée du bouclage (entités `Bouclage ECS - Programmation…`)
est actualisée par une relecture ciblée du bloc :
`BoostSequencer::take_refresh()`, `BoostRefreshRelay`, puis
`VissLinkComponent::request_block_refresh()`. Sans elle, l'affichage
attendait le cycle suivant, jusqu'à 6 h plus tard.

- **Quand** : la plage temporaire est confirmée (`ACTIVE`), ou le bloc
  d'origine est relu après une restauration ou une récupération (vérifiée
  ou déjà conforme).
- **Pas d'actualisation** dans ces cas :
  - écriture ignorée par la régulation (bloc inchangé) ;
  - relecture illisible ou bloc inattendu (récupération nécessaire) ;
  - refus avant écriture ;
  - sauvegarde oubliée.
- **Ce n'est pas une vérification.** La relecture des 8 octets juste après
  chaque écriture reste obligatoire, indépendante et seule décisive. Elle
  suit toujours immédiatement l'écriture, sans aucune trame entre les deux.
  L'actualisation ne décide de rien : elle met seulement l'affichage à jour.
- **Pas de lecture redondante** :
  - l'actualisation n'est demandée qu'au repos du séquenceur (`IDLE`, ou
    `ACTIVE` en attente), et sans autre commande en attente (A1/M2,
    consignes) ;
  - elle n'est jamais servie pendant une vérification ;
  - si une restauration commence avant qu'elle soit servie, elle est
    retirée sans rien émettre ;
  - elle est abandonnée après 60 s si le registre reste occupé (le cycle
    normal prend le relais) ;
  - sans entité de programmation du bouclage déclarée, rien n'est lu.

Séquence simulée sur `0x2230` :

- **plage** : lecture, écriture, vérification, actualisation ;
- **restauration** : relecture, écriture, vérification, actualisation.

L'affichage est à jour 0,3 à 7,5 s après la confirmation (resynchro de 2 à
4 s).

## 5 bis. Durée du bouclage (v0.5.2)

**Règle : 10 minutes demandées = 10 minutes de plage, à la précision des
échanges Optolink près.**

### Constat (v0.5)

Une demande de 10 min vers 21:27 a créé la plage 21:20–21:50, avec une
restauration prévue à 21:52. En v0.5, la restauration attendait la **fin de
la plage + 2 min**. Or la plage est arrondie à la dizaine : début vers le
bas, fin vers le haut après une marge de 2 min. Le bouclage durait donc
entre 12 et 24 min pour 10 demandées.

La plage 21:20–21:50 correspond à 10 min demandées si l'horloge de la
Vitotronic indiquait plus de 21:28:00 (21:28:01 + 12 min = 21:40:01,
arrondi à 21:50), ou à 20 min demandées. Le journal tranche
(`Circulation boost requested (10 min)`). En v0.5, l'entité « Durée »
revenait à 20 min à chaque redémarrage de l'ESP ; depuis la v0.5.2, elle
revient à **10 min**.

### Correction

- La plage calculée est inchangée : elle contient l'heure courante et couvre
  la durée demandée + 2 min, arrondie à la dizaine (§ 4). Ce n'est plus
  qu'une **fenêtre**.
- Une **minuterie monotone** (`millis()`) démarre quand la relecture
  confirme la plage temporaire. À l'expiration de la durée sélectionnée (10,
  20 ou 30 min), la restauration commence : relecture, puis écriture du bloc
  d'origine, puis relecture. La fin de la plage n'est plus attendue ;
  l'horloge `0x088E` n'est plus relue avant la restauration (une lecture de
  moins).
- La minuterie compare par différence signée (`time_reached`) : elle reste
  juste au débordement de `millis()` (tous les 49,7 jours).
- L'état affiche `Bouclage en cours : 10 min, arrêt vers 21:39 (plage
  21:20-21:50 programmée)`, puis `Programmation restaurée (vérifiée par
  relecture), après 10 min 00 s`.

Mesuré en simulation au niveau des trames KW, de la relecture qui confirme
la plage à l'écriture du bloc d'origine :

| Resynchro | 10 min | 20 min | 30 min |
|---|---|---|---|
| 2 s | 602,2 s | 1 203,6 s | 1 802,9 s |
| 3 s | 604,3 s | 1 203,6 s | 1 806,0 s |
| 4 s | 604,4 s | 1 207,8 s | 1 807,1 s |

L'écart (2 à 8 s) est l'attente de la synchronisation pour la relecture et
l'écriture de restauration. Il ne s'agit que de simulation : la durée réelle
du circulateur dépend aussi du temps de réaction de la régulation, à mesurer
(§ 5 ter).

### Liaison perdue, restauration refusée, redémarrage

| Situation | Comportement |
|---|---|
| Panneau modifié pendant la minuterie | constaté à l'expiration (bloc ≠ temporaire) : **jamais écrasé**, récupération nécessaire |
| Bus occupé à l'expiration (commande A1/M2, resynchro lente) | « restauration en cours », **pas d'alerte** |
| Liaison perdue à l'expiration (relecture impossible, trame non émise) | **fenêtre automatique de 15 min** après la fin de la durée : nouvelle relecture toutes les 30 s (la dernière à +15 min) ; restauration automatique seulement si la relecture donne **exactement** le bloc temporaire ; « restauration en cours », pas d'alerte |
| Liaison toujours absente à +15 min | **récupération nécessaire** : alerte, plus aucune tentative automatique, même si la liaison revient ; sauvegarde conservée ; bouton « Restaurer » ou correction sur le panneau |
| Écriture de restauration non appliquée | nouvelle relecture dans 2 min ; **au plus 3 écritures**, puis récupération nécessaire |
| Redémarrage de l'ESP pendant la plage ou la fenêtre | le temps écoulé est inconnu : **aucune minuterie relancée**, aucune écriture automatique ; récupération nécessaire, alerte ; le bouton « Restaurer » restaure **immédiatement** |

La sauvegarde n'est **jamais** effacée sans restauration confirmée par
relecture (ou constat que le bloc est déjà celui d'origine).

Entités :

- « Plage temporaire en cours » : de la confirmation à la fin de la
  restauration ;
- « Restauration en cours » (facultative, `restoring:`, v0.5.2) : durée
  écoulée, plage pas encore retirée, y compris les nouvelles tentatives de
  la fenêtre automatique. **Ce n'est pas une alerte** ;
- « Restauration nécessaire » : **seulement** quand une action manuelle est
  requise (récupération).

Quand la restauration ne peut pas se faire (récupération nécessaire), la
plage temporaire **reste dans la programmation hebdomadaire** : elle se
répétera chaque semaine ce jour-là. L'alerte reste levée et l'état indique
l'intervention, par exemple : `… ; sinon, supprimer sur le panneau la plage
21:20-21:50 du dimanche`. Après la correction sur le panneau, « Oublier la
sauvegarde » (aucune écriture).

## 5 ter. Mesure du circulateur par `0x0846` (étude)

`0x0846` est l'état de **sortie** du circulateur calculé par la régulation :
c'est la seule mesure du fonctionnement effectif. VissLink ne l'écrit
jamais, et cette étude ne change rien à cette règle.

| Usage | Intérêt | Risque | Décision |
|---|---|---|---|
| Démarrer la minuterie au passage de `0x0846` à `01` | durée de pompe exacte | si la régulation ne démarre pas la pompe (priorité ECS, blocage, autre logique), la minuterie ne démarre jamais : la plage n'est pas retirée et devient permanente | **écarté** |
| Prolonger la plage tant que la pompe n'a pas tourné 10 min | idem | durée non bornée, même risque | **écarté** |
| **Mesurer** seulement : délai confirmation → `01`, délai restauration → `00` | vérifie sur la chaudière que 10 min demandées = 10 min de pompe | aucun (lecture seule) | **retenu pour l'essai** |

En production, `0x0846` reste à 60 s (en commentaire), soit ±60 s : trop
grossier pour juger une durée de 10 min. Pour la phase 2 (§ 8), la
configuration
[`iot-viessmann-essai-bouclage-v0.5.2.yaml`](../examples/iot-viessmann-essai-bouclage-v0.5.2.yaml)
lit `0x0846` toutes les **15 s** (lecture seule, +3 lectures/min) et ajoute
l'entité « Restauration en cours ». Comparer dans l'historique Home
Assistant « Plage temporaire en cours » et « TEST — Bouclage ECS brut ».

Évolution possible, non implémentée : lecture ponctuelle de `0x0846` toutes
les 30 s par le séquenceur, **pendant la plage seulement**, avec les délais
mesurés dans l'état. Elle ne déciderait de rien. À envisager seulement si
l'historique ne suffit pas.

## 5 quater. Sept jours, affichage et appuis répétés (v0.7.1)

### Autorisation explicite

[`iot-viessmann-ecritures-autorisees.yaml`](../examples/iot-viessmann-ecritures-autorisees.yaml)
autorise les **six catégories d'écriture**, et le bouclage **les sept jours**
(`validated_days` : les 7 jours).

Les autres exemples ne changent pas : toutes les écritures y restent
désactivées et `validated_days: []`.

Rien n'est retiré dans le composant :

- les listes blanches restent en place ;
- les limites de température restent en place ;
- une seule écriture et une seule modification de consigne à la fois ;
- l'horloge est toujours recoupée avec Home Assistant ;
- la sauvegarde en flash est faite avant l'écriture ;
- l'écriture est vérifiée par relecture des 8 octets ;
- la restauration n'a lieu que si le bloc relu est exactement le bloc
  temporaire ;
- 15 min minimum entre deux déclenchements, 8 déclenchements par jour au
  plus.

**Risque accepté :** la correspondance bloc ↔ jour n'a été observée qu'un
dimanche (§ 3 et § 4). Sur un autre jour, si le bloc ne correspondait pas, la plage
serait inscrite sur un autre jour, et la pompe ne tournerait pas. Le bloc
serait quand même sauvegardé, relu et restauré à l'identique.

### Trois informations distinctes dans Home Assistant

| Entité | Source | Ce qu'elle dit |
|---|---|---|
| « Bouclage ECS — Plage inscrite » (`range`, facultative) | séquenceur | plage temporaire écrite dans la Vitotronic, par exemple `14:00-14:30 (dimanche)`, ou `Aucune`. Arrondie à la dizaine avec la marge : souvent plus longue que la durée demandée |
| « Bouclage ECS — Temps restant » (`remaining`, facultative, en min) | minuterie monotone | minutes avant la restauration automatique, comptées depuis la confirmation de l'écriture. Vaut 10 juste après la confirmation, 1 pendant la dernière minute, puis 0 |
| « Bouclage ECS — Pompe (état réel, 0x0846) » | `binary_sensor` visslink, 15 s | ce que fait réellement le circulateur. La régulation décide seule. Jamais écrit |

Les entités existantes ne changent pas : « Plage temporaire en cours »,
« Restauration en cours », « Restauration nécessaire » et « État de
l'opération ».

En cas de récupération, « Plage inscrite » indique « … peut-être encore
inscrite (récupération) » : sans relecture, l'état n'est pas connu.

### Durée et appuis répétés

- **Restauration :** 10 min demandées donnent une restauration 10 min après
  la confirmation de l'écriture par relecture, même si la plage inscrite
  dure 30 min.
- **Exemple :** une demande à 14:09:10 inscrit la plage 14:00-14:30, et la
  restauration a lieu vers 14:19 (tests
  `sim_v071_boost_10_min_in_a_30_min_range_restored_after_10_min` et
  `boost_range_and_remaining_are_distinct_displays`).
- **Appuis répétés :** un appui sur « Démarrer » pendant la plage est refusé
  (« opération déjà en cours (minuterie non prolongée) »), quelle que soit la
  durée choisie. La minuterie, la plage et la sauvegarde ne changent pas.
- **Après la restauration :** un nouvel appui est refusé pendant 15 min (test
  `boost_repeated_presses_never_extend_the_timer`).
- **Affichage d'un refus :** il reste 30 s dans « État de l'opération », ou
  jusqu'au changement d'état suivant. Ensuite, l'état réel réapparaît.

## 6. Coupures d'alimentation

Test `boost_power_cut_after_every_action` : coupure après **chacune** des
9 actions d'une opération complète, puis redémarrage.
`sim_v05_boost_power_cut_while_active_then_manual_restore` refait les
coupures au niveau KW (moteur, arbitre, ordonnanceur réinitialisés).

| Coupure | État au redémarrage | Résultat |
|---|---|---|
| avant la sauvegarde | aucune sauvegarde | rien n'a été écrit |
| après la sauvegarde, avant l'écriture | sauvegarde ; bloc = origine | récupération : constat, sauvegarde effacée, rien d'écrit |
| après l'écriture, **avant la relecture** | sauvegarde ; bloc = temporaire (ou origine) | récupération manuelle : restauration vérifiée (ou constat) |
| pendant la plage | sauvegarde ; bloc = temporaire | idem |
| pendant la restauration | sauvegarde ; bloc = origine ou temporaire | idem |
| après la restauration, avant l'effacement | sauvegarde ; bloc = origine | constat, sauvegarde effacée |

Dans tous les cas : aucune écriture automatique, et le bloc d'origine est
retrouvé après l'action manuelle.

Limites :

- la flash de l'ESP8266 ne peut pas être relue directement par l'API
  ESPHome. La vérification repose sur le code retour de `sync()` (effacement
  et écriture du secteur) et sur la relecture de la préférence ;
- l'emplacement de la préférence dépend de l'ordre d'allocation : **ne
  jamais changer de firmware tant qu'une récupération est en attente**. La
  sauvegarde pourrait ne plus être retrouvée ; les octets d'origine restent
  dans l'historique Home Assistant ;
- une coupure **pendant** l'effacement du secteur de flash peut perdre la
  sauvegarde. Elle est détectée par la somme de contrôle (sauvegarde
  ignorée), et le programme doit alors être vérifié sur le panneau.

## 7. Risques résiduels (bloquants pour la production)

1. **Acceptation de l'écriture de 8 octets par la KW3 : inconnue.**
2. **Format de la réponse à une écriture de 8 octets : incertain.**
   vcontrold attend 1 octet (`00`), VitoWiFi en attend 8. VissLink accepte
   `00` ; toute autre réponse est indéterminée et tranchée par la
   relecture. Si la KW3 répond 8 octets, chaque écriture sera journalisée
   « indéterminée », mais restera vérifiée.
3. **La régulation peut retrier ou recompacter le bloc** : la relecture ne
   serait alors plus identique. Le cas mène à « récupération requise » sans
   écriture, ce qui est sûr mais bloque la fonction.
4. **Convention du jour de la semaine de `0x088E`** non vérifiée sur la KW3,
   et correspondance bloc ↔ jour à valider jour par jour.
5. **Écriture interrompue** (trame tronquée) : comportement de la KW3 non
   documenté. Le cas est couvert par la relecture et la récupération.
6. **Restauration impossible** (liaison perdue durablement, ESP redémarré,
   restauration refusée) : la plage ajoutée se répétera chaque semaine ce
   jour-là, à cette heure, jusqu'à la correction. Elle est courte (≤ 40 min)
   et se termine d'elle-même. L'alerte « Restauration nécessaire » reste
   levée et l'état indique la plage à supprimer sur le panneau (§ 5 bis).
7. Usure (§ 5) et perte de la sauvegarde lors d'un changement de firmware
   (§ 6).

## 8. Validation sur la chaudière

### Phase 1 : lecture seule et simulation (sans aucune écriture)

Flasher l'exemple v0.5 **tel quel**, avec `enable_schedule_writes: false`
et `validated_days: []`. Aucune écriture n'est possible dans cet état.

1. **Horloge** : comparer « Vitotronic - Horloge » à l'afficheur (date,
   heure, jour). Si l'entité affiche `invalide (weekday does not match the
   date)` : convention du jour différente, **arrêt**, analyser la trame
   brute.
2. **Correspondance des jours** : avec le diagnostic
   `examples/iot-viessmann-diagnostic-bouclage.yaml` (§ 5.3 de
   `docs/etude-bouclage-ecs.md`), modifier depuis le panneau une plage d'un
   seul jour et vérifier que seul son bloc change. Recommencer pour chaque
   jour à valider (au minimum les jours d'utilisation prévus).
3. **Programme actuel** : vérifier que le bloc du jour est trié et compact,
   et rétablir sur le panneau la plage 21:40–21:50 si l'essai du 15/11 l'a
   supprimée.
4. **Calcul** : pour l'heure de l'essai, comparer la plage que VissLink
   calculerait (§ 4) avec ce qui est attendu.
5. **Simulation** : `make cpp-test` exécute toute la machine à états, avec
   les coupures, sans rien émettre vers la chaudière.

### Phase 2 : premier essai d'écriture — préparé, **NON exécuté**

À faire seulement après une **validation explicite**, sur place, un jour
validé en phase 1.

**Retour arrière indépendant de l'ESP**, préparé avant l'essai :

- noter sur papier le programme de bouclage du jour, tel qu'affiché par le
  panneau (actuellement `06:10–08:30, 21:40–21:50`) et les 8 octets bruts
  relus ;
- savoir le ressaisir sur le panneau. C'est le retour arrière **de
  référence**, valable même si l'ESP est hors service.

Déroulement :

1. Configuration d'essai : partir de
   `examples/iot-viessmann-essai-bouclage-v0.5.2.yaml` (`0x0846` à 15 s,
   écritures désactivées dans le fichier) et mettre à la main
   `enable_writes: true`, `enable_schedule_writes: true`,
   `validated_days: [<jour validé>]`, les autres options d'écriture à
   `false`, aucune consigne modifiable. Compiler
   et vérifier l'avertissement `SCHEDULE BLOCK WRITES ENABLED`, puis flasher.
2. Hors de toute plage programmée, au moins 30 min avant 23:50 : choisir
   **10 min** puis appuyer **une seule fois** sur « Bouclage ECS — Démarrer
   maintenant ».
3. Suivre le journal :
   - `Backup of 0x22xx (...) saved to flash` ;
   - `0x22xx -> ... (schedule block write, experimental)` ;
   - l'accusé, ou une issue « indéterminée » ;
   - la relecture ;
   - l'état « Bouclage en cours : 10 min, arrêt vers hh:mm (plage
     hh:mm-hh:mm programmée) ».
4. Vérifier **sur le panneau** que la plage apparaît, puis **physiquement**
   que le circulateur démarre, et que `0x0846` passe à `1`. Noter l'heure
   de la confirmation et celle du démarrage (§ 5 ter).
5. Attendre la restauration automatique, **10 min après la confirmation**
   (et non à la fin de la plage) : « Programmation restaurée (vérifiée par
   relecture), après 10 min 0x s ». Noter l'heure de l'arrêt du
   circulateur. Vérifier sur le panneau que le programme est **identique
   au relevé papier**.
6. **Au moindre écart** (refus, relecture différente, état « Récupération
   nécessaire », panneau incohérent) : ne plus rien déclencher,
   `enable_schedule_writes: false`, **ressaisir le programme sur le
   panneau** à partir du relevé papier, puis « Oublier la sauvegarde ».
7. Reporter les résultats (trames, accusé, délais, panneau, circulateur)
   dans ce document. Ensuite seulement, décider d'un usage régulier.

Ne pas considérer l'essai manuel du 15 novembre, ni les simulations, comme
une validation de l'écriture Optolink.

## 9. Mesures

| Élément | Résultat |
|---|---|
| Tests C++ (ASan/UBSan) | 193 réussis (dont 33 pour la v0.5) |
| Tests pytest | 129 réussis |
| `make compat` | 7 configurations compilent (v0.1, v0.2, v0.2.2, diagnostic v0.2.2, v0.3, v0.4, v0.4.1) |
| Exemple v0.4.1 compilé en v0.5 | 35 536 o RAM / 450 181 o Flash (v0.4.1 : 35 284 / 447 985) |
| Exemple v0.5 (bouclage déclaré, inactif) | 37 668 o RAM (46,0 %) / 468 673 o Flash (44,9 %) |
| Exemple v0.5 + états Éco/Party (4 `binary_sensor`) | **37 952 o RAM (46,3 %) / 469 085 o Flash (44,9 %)** |
| Exemple v0.5, écritures de programmation activées | identique (simples indicateurs) |
| Diagnostic bouclage (lecture seule) | 36 188 o RAM / 450 881 o Flash |

Le composant de bouclage coûte ~1,9 Ko de RAM, surtout pour les 7 entités
ESPHome ; ses messages sont stockés en flash (`PSTR`). Sans
`visslink_boost`, la v0.5 n'ajoute que ~250 o de RAM.
