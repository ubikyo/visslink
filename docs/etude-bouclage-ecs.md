# Étude : marche ponctuelle du bouclage ECS (Vitotronic 300 KW3)

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


> **Suite : implémentation expérimentale v0.5 (piste 2), désactivée par
> défaut, décrite dans [`bouclage-ecs-v0.5.md`](bouclage-ecs-v0.5.md).**
>
> **Statut de cette étude : étude technique** (elle n'ajoutait aucune
> écriture ; la v0.5 en a ajouté une, expérimentale et désactivée par défaut).

Objectif : un bouton Home Assistant « Lancer le bouclage ECS », idéalement
pour 5 minutes, sans modifier durablement les programmations.

## Synthèse

| Piste | Écriture KW documentée | Agit via | Durée de 5 min | Restauration | Verdict |
|---|---|---|---|---|---|
| 1. Écriture de `0x0846` | vcontrold, Vitotronic 200 **KW2** seulement ; aucun usage confirmé | l'état de **sortie** de la pompe | non maîtrisée (la régulation recalcule la sortie) | sans objet | **écartée** (vérification finale, § 5) |
| 2. Plage ajoutée dans `0x2200..0x2230` | vcontrold, **KW2** (`setTimerZirku*`, 8 octets) ; format déjà décodé par VissLink | le **programme** de la régulation | **non** : pas de 10 min ; 10 à 30 min | indispensable (2 écritures par usage) | **seule piste Optolink réaliste**, sous conditions |
| 3. Schaltmodul-V (entrée externe) | aucune adresse Optolink trouvée | une **entrée câblée** du module | selon le codage du module (non vérifié pour la KW3) | aucune | **hors Optolink** : contact sec piloté par l'ESP, si le module est présent |

Recommandation : valider d'abord, **sans aucune écriture Optolink**, que la
régulation démarre bien le bouclage lorsqu'une plage est ajoutée depuis son
panneau (§ 2.6). Ensuite seulement, implémenter la piste 2 derrière une
autorisation dédiée (§ 4). Si le Schaltmodul-V est installé, la piste 3 par
contact sec est plus simple et plus sûre.

---

## 1. Écriture directe de `0x0846`

### Sources

| Source | Contenu | Compatibilité KW3 |
|---|---|---|
| vcontrold `xml/kw/vito.xml` (commit `f66b465`) | `getPumpeStatusZirku` et `setPumpeStatusZirku` : `0x0846`, 1 octet, unité `RT` (`00`/`01`), appareil par défaut **V200KW2** (`2098`) | protocole KW identique ; **appareil différent** |
| vcontrold `xml/300/vito.xml` | même `setPumpeStatusZirku`, dans un fichier qui mélange KW2 et P300 | aucune indication d'appareil |
| Configuration openv pour Vitotronic 150 KB1 (citée dans la demande) | `setPumpeStatusZirku` sur `0x0846` | **non retrouvée** dans les sources consultées ; la KB1 est une autre famille de régulation, probablement P300 |
| Wiki openv « Adressen » | `Zirkulationspumpe 0x0846, 0..1, 1 octet`, section « Zustände Pumpen, Mischer, etc. » **sans** mention lecture/écriture | — |

Aucune source ne décrit l'**effet** d'une écriture : pas de durée, pas de
retour au fonctionnement automatique, pas de témoignage d'utilisation.

### Analyse

- `0x0846` est un **état de sortie** : la valeur que la régulation calcule
  pour le relais de la pompe. Sur la KW3, VissLink ne l'a encore confirmé
  qu'en lecture partielle (diagnostic v0.2.2, validation en cours).
- Trois comportements possibles d'une écriture `01`, aucun documenté :
  1. refus (`05`) ou accusé sans effet : sans risque, sans intérêt ;
  2. valeur écrasée au cycle suivant de la régulation : marche de quelques
     secondes au mieux ;
  3. sortie forcée **jusqu'à une prochaine décision de la régulation**, durée
     inconnue, sans commande d'arrêt fiable (écrire `00` pourrait forcer
     l'arrêt contre le programme, ou une protection).
- Le cas 3 contourne la logique de la régulation (programmes, priorités,
  éventuelle protection antilégionelle ou antigel). C'est contraire au
  principe de VissLink : ne jamais contourner les contrôles de la Vitotronic.

### Faisabilité avec le moteur actuel

Techniquement triviale : écriture d'un octet, identique aux consignes. Elle
est **délibérément non proposée** : ajouter `0x0846` à une liste blanche
n'est pas justifié tant que l'effet n'est ni documenté pour une KW ni
observé.

### Validation sans écriture

1. Terminer la validation **en lecture** de `0x0846` (procédure v0.2.2,
   `docs/registres.md` § 3.1) : valeurs `00`/`01` et concordance avec la
   pompe réelle.
2. Rien d'autre n'est possible sans écrire : l'effet d'une écriture ne se
   constate qu'en écrivant.

**Décision proposée : ne pas ajouter `0x0846` à la liste blanche.** À
réexaminer seulement si une source décrit l'effet d'une écriture sur une
régulation KW (durée, retour en automatique).

---

## 2. Plage temporaire dans la programmation du bouclage (`0x2200`)

### 2.1 Sources

| Source | Contenu | Compatibilité KW3 |
|---|---|---|
| vcontrold `xml/kw/vito.xml` | `getTimerZirku*` et **`setTimerZirku*`** : `0x2200` (lundi) à `0x2230` (dimanche), **8 octets**, unité `CT` ; idem `setTimerM1/M2/WW` | KW2 ; mêmes adresses que celles lues sur la KW3 |
| vcontrold `src/unit.c`, `setCycleTime` | bloc pré-rempli de 8 × `FF`, puis paires (début, fin) : `heure << 3 | minutes / 10` | **identique** au format décodé par VissLink depuis la v0.2.1 |
| Wiki openv « Adressen » | `0x20..` / `0x30..` (programmations des circuits) dans « Bedienung (read write) » | générique |
| vcontrold `getSystemTime` / `setSystemTime` | `0x088E`, 8 octets BCD : siècle, année, mois, jour, jour de semaine, heure, minute, seconde | KW2 ; utile pour connaître l'heure **de la régulation** |
| VissLink (KW3 réelle) | les 7 blocs des 4 circuits sont lus et décodés correctement ; correspondance de chaque bloc avec son jour **non vérifiée jour par jour** | confirmé en lecture |

### 2.2 Format d'un bloc (rappel)

```text
DEBUT1 FIN1 DEBUT2 FIN2 DEBUT3 FIN3 DEBUT4 FIN4      (8 octets)
octet = heure × 8 + minutes / 10     FF FF = plage libre
```

Bloc actuel du bouclage relevé sur la chaudière : `31 43 AC AD FF FF FF FF`
(06:10-08:30, 21:40-21:50).

Contraintes :

- **Résolution de 10 minutes.** Une marche de 5 minutes est impossible.
  Une plage ajoutée à l'instant `t` couvre `[⌊t⌋₁₀, ⌊t⌋₁₀ + D]`, soit une
  marche effective comprise entre `D − 10` et `D` minutes. Pour au moins
  10 minutes réelles, il faut `D = 20`.
- **4 plages par jour au plus.** Avec 2 plages déjà utilisées, il en reste
  2. Si une plage voisine existe, il vaut mieux l'étendre que d'en ajouter.
- **Pas de plage traversant minuit** (non documenté) : refus entre 23:40 et
  00:00.
- Ordre des plages : conserver un ordre croissant sans chevauchement. On
  ignore si la régulation l'exige ; c'est la forme produite par son
  panneau.
- **Jour courant selon l'horloge de la régulation**, pas celle de Home
  Assistant : lecture de `0x088E` avant tout calcul. Les deux horloges
  doivent concorder (même jour, écart de quelques minutes au plus), sinon
  refus.

### 2.3 Séquence envisagée

1. Demande « Lancer le bouclage » (bouton Home Assistant).
2. Lecture de l'horloge `0x088E` et **relecture fraîche** du bloc du jour
   (pas la valeur en cache).
3. Si une plage est déjà active : rien à écrire.
4. Calcul du nouveau bloc : ajout ou extension d'**une seule** plage de
   durée `D` (10 à 30 min). Contrôles : décodable, ≤ 4 plages, ordonné, pas
   de passage de minuit, les autres octets inchangés.
5. Sauvegarde du bloc d'origine (RAM et flash de l'ESP), état « à
   restaurer ».
6. Écriture des 8 octets (une trame, jamais réémise), puis relecture et
   comparaison des 8 octets.
7. À l'heure de fin, relecture du bloc. **S'il est identique au bloc
   modifié**, écriture du bloc d'origine, puis relecture. S'il a changé
   (modification depuis le panneau), **aucune restauration** : alerte.
8. Fin : état « restauré » ou alerte « programmation à vérifier ».

### 2.4 Faisabilité avec le moteur KW actuel

| Élément | État actuel | Travail nécessaire |
|---|---|---|
| Trame d'écriture | `01 F4 AH AL LEN D…`, `MAX_WRITE_LENGTH = 1` | autoriser 8 octets **seulement** pour la liste blanche des programmations ; tampon déjà de 8 octets en lecture |
| Réponse à une écriture de 8 octets | **non établie** : vcontrold attend 1 octet (`RECV 1 SR`), VitoWiFi en attend 8 | accepter `00` sur 1 octet ; toute autre réponse (8 octets, `05`…) = issue indéterminée, la relecture tranche ; jamais de réémission |
| Arbitre | une valeur de 1 octet par demande | demande de 8 octets ; une seule à la fois |
| Relecture | lecture de 8 octets d'un bloc (déjà faite pour les programmations) | relecture urgente du bloc écrit (`expedite` ne gère qu'un bloc : à adapter) |
| Horloge | non lue | lecture de `0x088E` (8 octets BCD) |
| Restauration différée | inexistante | minuterie et état persistant (préférences ESPHome) |
| Charge | — | 2 lectures + 2 écritures + 2 relectures par usage : négligeable |

### 2.5 Risques et protections

| Risque | Protection proposée |
|---|---|
| Écriture dans le mauvais jour (correspondance non vérifiée jour par jour) | validation préalable de la correspondance (§ 2.6) ; jour pris dans `0x088E` et recoupé avec l'heure de Home Assistant |
| Programmation modifiée **durablement** (redémarrage de l'ESP ou perte de liaison avant la restauration) | bloc d'origine sauvegardé en flash ; au démarrage, **aucune écriture automatique** : alerte « restauration en attente » et bouton « Restaurer » (règle de la v0.3) ; au pire, la plage ajoutée se répète chaque semaine ce jour-là, à cette heure-là |
| Écrasement d'une modification faite depuis le panneau | restauration seulement si le bloc relu est exactement le bloc écrit |
| Bloc invalide (paire incomplète, minute impossible) | bloc décodé avant envoi ; seules l'ajout ou l'extension d'une plage sont permis |
| Usure de la mémoire de la régulation | 2 écritures par usage ; espacement minimal entre deux usages (par exemple 30 min) |
| Réponse ambiguë (1 ou 8 octets) | jamais de réémission ; relecture des 8 octets |
| Écriture dans un autre programme (A1, M2, ECS) | liste blanche **distincte**, limitée à `0x2200..0x2230`, et autorisation dédiée `enable_schedule_writes` (défaut `false`, exige `enable_writes`) |
| Restauration impossible (liaison absente à l'heure de fin) | nouvelles tentatives de **restauration** bornées (par exemple 3 sur 30 min), puis alerte ; la plage ajoutée se termine d'elle-même à l'heure prévue |

### 2.6 Validation sans écriture Optolink

1. **Mécanisme** : depuis le **panneau** de la Vitotronic, ajouter au jour
   courant une plage de bouclage commençant à la prochaine dizaine de
   minutes. Observer la pompe (bruit, température du retour), `0x0846` (si
   déclaré en diagnostic) et le délai de démarrage. Supprimer ensuite la
   plage depuis le panneau. Cette étape confirme qu'une plage ajoutée en
   cours de journée démarre bien le bouclage. Si ce n'est pas le cas, la
   piste 2 est abandonnée.
2. **Correspondance des jours** : pendant le même essai, vérifier que seul le
   bloc du jour (`0x2200 + 8 × jour`) change dans les journaux DEBUG
   (`0x2218 <- …` pour un jeudi). Programmation lue toutes les 6 h : réduire
   temporairement `update_interval` à 10 min pour l'essai.
3. **Horloge** : lire `0x088E` (8 octets) et comparer à l'heure affichée.
   Aucune entité actuelle ne publie 8 octets bruts. Il faudrait un
   `text_sensor` de diagnostic en lecture seule (`type: raw`, hexadécimal),
   seul ajout de code nécessaire à cette validation.
4. **Ordre des plages** : relever dans les journaux la forme exacte d'un bloc
   modifié depuis le panneau (plages triées, `FF` en fin), pour la
   reproduire.

---

## 3. Schaltmodul-V : entrée externe

### Sources

- Les informations de la demande (entrée DE7, marche ponctuelle du
  bouclage, durée réglée par le codage 74) **n'ont pas pu être confirmées**
  pour la Vitotronic 300 KW3 dans les documents accessibles. Les recherches
  ont trouvé la référence du Schaltmodul-V (7143 513) dans une liste
  d'accessoires, et des fonctions de codage de pompe de bouclage pour
  d'autres régulations, mais aucune notice associant KW3, DE7 et codage 74.
- Aucune adresse Optolink correspondant à une « marche ponctuelle du
  bouclage » ou à l'entrée d'un module n'apparaît dans vcontrold ni sur le
  wiki openv.

### Faisabilité

- **Par Optolink : non**, faute d'adresse documentée. Écrire dans les
  codages (adresses de service) par Optolink est exclu : risque élevé,
  aucune source.
- **Hors Optolink : oui, si le module est installé et codé.** La fonction se
  déclenche par un contact sur l'entrée du module. L'ESP peut fermer ce
  contact par un relais (contact sec, isolé galvaniquement) piloté par un
  `switch` ou un `button` ESPHome natif, avec une impulsion de durée fixe.
  Pas de moteur KW, pas d'écriture dans la régulation, pas de restauration :
  la régulation gère elle-même la durée.

### Risques et protections

- Câblage sur le bornier du module : à faire par une personne qualifiée,
  hors tension, selon la notice du module.
- Impulsion bornée (par exemple 1 s), jamais de maintien. Aucun
  déclenchement au démarrage (`restore_mode: ALWAYS_OFF`).

### Validation sans écriture

1. Vérifier sur place si un Schaltmodul-V est installé et raccordé au
   KM-BUS de la KW3.
2. Lire dans le menu de codage de la régulation (codage 2) les adresses
   concernées, avec la notice du module et de la KW3.
3. Tester la fonction avec un **bouton-poussoir manuel** sur l'entrée, avant
   tout raccordement de l'ESP.

---

## 4. Architecture proposée (piste 2, si la validation § 2.6 réussit)

```yaml
visslink:
  enable_writes: false
  enable_schedule_writes: false     # nouvelle autorisation, défaut false

button:
  - platform: visslink
    name: "Lancer le bouclage ECS"
    type: circulation_boost
    schedule_address: 0x2200        # liste blanche : bouclage uniquement
    duration: 20min                 # 10, 20 ou 30 min (résolution de 10 min)

binary_sensor:                      # créé par la plateforme
  # « Bouclage ECS - Restauration en attente » (problème)
```

| Composant | Rôle |
|---|---|
| `write_guard.h` | `SCHEDULE_WRITE_WHITELIST = {0x2200..0x2230}`, `plan_circulation_boost(bloc, heure, durée)` → nouveau bloc ou refus motivé, `is_single_range_addition(avant, après)` |
| `KwProtocol` | écriture jusqu'à 8 octets pour cette liste blanche seulement ; réponse acceptée : `00` sur 1 octet ; tout le reste = indéterminé |
| `BoostController` (nouveau, testable sur PC) | machine à états : lecture horloge → lecture bloc → écriture → relecture → attente → relecture → restauration → relecture ; une session à la fois ; espacement minimal ; tentatives de restauration bornées |
| Persistance | bloc d'origine + jour + heure de fin dans les préférences ESPHome ; au démarrage, alerte uniquement |
| Arbitre existant | les écritures passent par `WriteArbiter` (une à la fois, pas de réémission) ; la priorité et la préemption de la v0.4.1 s'appliquent |
| Tests | simulateur : blocs, horloge, 4 plages pleines, minuit, panneau modifié pendant la session, redémarrage avant la restauration, liaison perdue à l'heure de fin, réponse 1 ou 8 octets |

### Points à décider avant l'implémentation

1. Une restauration **automatique au démarrage** serait une exception à la
   règle « aucune écriture au démarrage ». Proposition : non, alerte
   seulement.
2. Durée par défaut : 20 min (au moins 10 min de marche garanties).
3. Le résultat de la validation du § 2.6 conditionne tout le reste.

## Sources

- vcontrold : <https://github.com/openv/vcontrold> (commit `f66b465`,
  `xml/kw/vito.xml`, `xml/300/vito.xml`, `src/unit.c`)
- Wiki openv, « Adressen » : <https://github.com/openv/openv/wiki/Adressen>
- Wiki openv, « Protokoll KW » : <https://github.com/openv/openv/wiki/Protokoll-KW>
- VitoWiFi : <https://github.com/bertmelis/VitoWiFi> (réponse de N octets
  attendue pour une écriture de N octets)
- Recherches sans résultat concluant sur Schaltmodul-V, DE7 et codage 74 pour
  la KW3 (notices Viessmann d'autres régulations, communauté Viessmann).

---

## 5. Décision : écriture de `0x0846` écartée, diagnostic en lecture seule

### 5.1 Vérification finale de `0x0846`

| Question | Résultat |
|---|---|
| Écriture réellement documentée ? | **Non.** `setPumpeStatusZirku` n'est qu'une **entrée de configuration** de vcontrold (`0x0846`, 1 octet, unité `RT`), présente depuis l'import initial du dépôt (2017), sans description de l'effet, sans ticket ni discussion retrouvés. Le wiki openv liste `0x0846` sans indication lecture/écriture. Aucun témoignage d'utilisation trouvé. |
| Modèles concernés | Fichier KW de vcontrold : appareil par défaut **Vitotronic 200 KW2** (`2098`). Fichier « 300 » : mélange de KW2 et P300 (Vitodens 200 HO1, `20CB`), sans précision. Configuration Vitotronic 150 KB1 citée : non retrouvée. **Aucune source pour la 300 KW3** (`209C`). |
| Valeurs acceptées | Unité `RT` de vcontrold : `00` → « 0 », `01` → « 1 », `03` → « 2 » (pompe de circuit en mode réduit, Vitodens 200 HO1). C'est une table de **conversion** du logiciel, pas une plage validée par la régulation. |
| Effet temporaire ou persistant ? | **Inconnu.** Aucune source. `0x0846` est un **état de sortie** recalculé par la régulation : effet nul, de courte durée, ou forçage d'une durée inconnue. |
| Méthode documentée pour rendre la main à la régulation | **Aucune.** Pas de mode « automatique » ni de registre de libération documenté. Écrire `00` pourrait aussi bien forcer l'arrêt. |

**Conclusion : l'essai d'écriture sur `0x0846` est écarté.** Aucune
commande d'écriture n'est développée. `0x0846` n'est pas ajouté à la liste
blanche. Il reste lu en octet brut, pour le diagnostic.

### 5.2 Diagnostic en lecture seule des plages `0x2200`

Configuration :
[`examples/iot-viessmann-diagnostic-bouclage.yaml`](../examples/iot-viessmann-diagnostic-bouclage.yaml),
identique à l'exemple v0.4.1 sauf :

- programmation du bouclage relue toutes les **10 min** (au lieu de 6 h),
  avec un capteur texte par jour (`TEST — Bouclage lundi (0x2200)` …
  `dimanche (0x2230)`) ;
- `TEST — Bouclage ECS brut` : octet brut de `0x0846`, toutes les 60 s ;
- les trois autorisations d'écriture restent à `false` : **aucune écriture
  Optolink** n'est possible (tests
  `test_loop_diagnostic_is_read_only_and_valid` et
  `test_loop_diagnostic_only_adds_read_entities`).

Charge simulée : ~9,2 lectures/min (exemple : ~7,6). Brûleur à 30 s et
températures à 60 s inchangés de 2 à 4 s de resynchro. Les 7 blocs du
bouclage sont relus environ toutes les 10 min. Mémoire : 35 944 o de RAM
(43,9 %), 448 701 o de Flash (43,0 %).

Les journaux DEBUG affichent les trames brutes de chaque bloc
(`0x2218 <- 31 43 AC AD FF FF FF FF`). Enregistrer le journal pendant
l'essai :

```sh
esphome logs examples/iot-viessmann-diagnostic-bouclage.yaml | tee essai-bouclage.log
```

### 5.3 Procédure : plage ajoutée depuis le panneau

Toutes les modifications se font **sur le panneau de la Vitotronic**.
VissLink ne fait que lire.

1. **Préparation.** Flasher la configuration de diagnostic. Attendre une
   lecture complète des 7 jours (≈ 4 min après le démarrage). Noter :
   - la programmation du bouclage affichée par la régulation pour le jour
     courant ;
   - les 7 capteurs `TEST — Bouclage …` et la trame brute du jour dans le
     journal ;
   - l'état de `TEST — Bouclage ECS brut` (hors plage, attendu `0`) et
     l'état réel de la pompe (bruit, température du retour de bouclage).
2. **Choisir un moment hors des plages existantes** (actuellement
   06:10-08:30 et 21:40-21:50), au moins 20 minutes avant la prochaine
   dizaine de minutes visée, et noter l'heure de la régulation.
3. **Ajouter sur le panneau**, pour le **jour courant uniquement**, une
   plage de 10 ou 20 minutes commençant à la prochaine dizaine de minutes
   (par exemple 14:30-14:50). Noter l'heure exacte de la validation.
4. **Observer**, en notant les heures :
   - le capteur du jour change à la relecture suivante (≤ ~13 min) ; **les
     six autres jours ne changent pas** : la correspondance bloc ↔ jour est
     alors confirmée pour ce jour ;
   - la trame brute : forme exacte du bloc écrit par la régulation (ordre
     des plages, `FF` en fin, valeur des octets) ;
   - au début de la plage : démarrage réel de la pompe, et passage de
     `0x0846` à `1` (délai par rapport à l'heure de début) ;
   - à la fin de la plage : arrêt de la pompe et retour de `0x0846` à `0`.
5. **Supprimer la plage sur le panneau**, puis vérifier à la relecture
   suivante que la trame du jour est **identique à celle de l'étape 1**.
6. **Variante utile** : ajouter une plage **déjà commencée** (par exemple
   14:20-14:40 alors qu'il est 14:27). La pompe démarre-t-elle aussitôt, ou
   seulement à la prochaine plage ? C'est le cas d'usage d'un bouton
   « lancer maintenant ».
7. **Ne pas** modifier un autre jour, un autre circuit, ni l'ECS pendant
   l'essai.

### 5.4 Résultats attendus et suite

| Observation | Conséquence pour la piste 2 |
|---|---|
| La pompe démarre au début de la plage ajoutée, `0x0846` suit | mécanisme confirmé ; noter le délai |
| La pompe démarre aussitôt pour une plage déjà commencée (étape 6) | un bouton « lancer maintenant » est possible |
| Elle ne démarre qu'au début d'une plage future | le bouton devra viser la prochaine dizaine de minutes (attente jusqu'à 10 min) |
| Seul le bloc du jour change | correspondance bloc ↔ jour confirmée pour ce jour |
| Un autre bloc change, ou aucun | **arrêt** : la correspondance n'est pas celle attendue |
| La pompe ne démarre pas | **piste 2 abandonnée** |
| `0x0846` ne suit pas la pompe | `0x0846` reste non confirmé (sans effet sur la piste 2) |

Aucune implémentation d'écriture de programmation n'est engagée avant ces
résultats et une nouvelle décision.
