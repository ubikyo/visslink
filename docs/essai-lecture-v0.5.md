# Essai en lecture seule — VissLink v0.5

> **v1.0.0 :** document historique. Les options d'autorisation (`enable_*`,
> `allow_write`, `validated_days`), les entités « État modification / commande »
> et les exemples intermédiaires cités ici n'existent plus. État actuel :
> `docs/v1.0.0.md` et `examples/iot-viessmann-v1.0.0.yaml`.


Configuration :
[`examples/iot-viessmann-essai-lecture-v0.5.yaml`](../examples/iot-viessmann-essai-lecture-v0.5.yaml).

Objectif : vérifier sur la chaudière, **sans aucune écriture Optolink** :

- l'horloge `0x088E` ;
- les états Éco et Party de A1 et M2 ;
- les programmations du bouclage (correspondance bloc ↔ jour) ;
- l'état du circulateur de bouclage `0x0846`.

Les quatre autorisations d'écriture sont à `false` et `validated_days` est
vide : aucun calendrier, mode ou consigne ne peut être écrit. Les
fonctions existantes (lectures, entités `number` et `select`, bouclage à la
demande) restent présentes, mais toute commande est refusée.

## 0. Avant de flasher

1. **Relevé papier** (retour arrière indépendant de l'ESP) :
   - programmation du bouclage de chaque jour, telle qu'affichée par le
     panneau ;
   - modes A1 et M2, consignes, état Éco et Party sur le panneau et sur la
     Vitotrol 200.
2. **Programme du dimanche** : l'essai du 15/11 a laissé
   `06:10–08:30, 14:00–14:10`. Rétablir sur le panneau
   `06:10–08:30, 21:40–21:50`, si c'est bien le programme souhaité.
3. Compiler sans flasher :

   ```sh
   esphome compile examples/iot-viessmann-essai-lecture-v0.5.yaml
   ```

   Vérifier qu'aucune ligne `ENABLED` n'apparaît (ni `WRITES ENABLED`, ni
   `SCHEDULE BLOCK WRITES ENABLED`).
4. Flasher (OTA) **manuellement**, puis enregistrer le journal pendant toute
   la durée de l'essai :

   ```sh
   esphome logs examples/iot-viessmann-essai-lecture-v0.5.yaml | tee essai-v05.log
   ```

## 1. Délai de resynchronisation (charge)

Les quatre Éco/Party sont lus toutes les 60 s, comme en production depuis
la v0.5.1 : ~13 lectures/min au lieu de ~17. Simulé : brûleur lu
780 fois sur 780 en 6 h 30 à 2 et 3 s de resynchro. À 4 s, 719 lectures au
lieu de 593 avec Éco/Party à 30 s : environ 8 % de lectures du brûleur
retardées.

- Dans la première minute du journal, mesurer l'écart entre deux lignes
  ` <- ` consécutives. Au-delà de 2 s, cet écart est le délai de
  resynchro réel.
- **Si l'écart dépasse 3 s**, le noter : c'est la limite au-delà de
  laquelle la liaison devient chargée.
- Pendant tout l'essai, « Brûleur actif » doit rester lu toutes les ~30 s
  (lignes `0x0842 <- 0x`).

## 2. Horloge `0x088E`

- Comparer « Vitotronic - Horloge » (catégorie diagnostic) à l'afficheur :
  date, heure à la minute près, jour de la semaine.
- Relever la trame brute (`0x088E <- 20 26 11 …`), dont le 5ᵉ octet est le
  jour de la semaine.
- **Si l'entité affiche `invalide (weekday does not match the date)`**, la
  convention du jour n'est pas celle de vcontrold : noter la trame et ne
  pas aller plus loin pour le bouclage à la demande.
- Résultat à noter : l'écart en minutes avec Home Assistant (le bouclage
  exigera moins de 5 min).

## 3. Éco et Party

Procédure détaillée : [`registres.md` § 6](registres.md).

1. Au repos : relever les quatre états (attendu : tous inactifs, `00`).
2. Vitotrol 200 : activer puis désactiver **Éco**. `0x3302` doit passer à
   `01`, puis revenir à `00`. Attendre au moins une minute à chaque fois.
3. Vitotrol 200 : activer puis désactiver **Party**. Même vérification sur
   `0x3303`. Noter si Party s'arrête de lui-même.
4. Pendant ce temps, `0x2302` et `0x2303` (A1) ne doivent pas changer.
5. A1 : tester seulement depuis une commande associée à A1 (panneau).
6. Toute entité « inconnue » (avertissement `unexpected binary value`)
   signifie une valeur autre que `00`/`01`. Noter l'octet : l'entité devra
   devenir un capteur brut.

## 4. Programmations du bouclage et circulateur

1. Après ≈ 4 min, les 7 entités `TEST — Bouclage lundi (0x2200)` …
   `dimanche (0x2230)` sont remplies. Les comparer au relevé papier.
2. **Correspondance des jours** : sur le panneau, modifier la programmation
   du bouclage d'**un seul jour** (par exemple ajouter une plage de
   10 minutes plus tard dans la journée). Au plus tard 13 min après,
   **seul** le capteur de ce jour doit changer. Noter la trame brute.
   Rétablir le programme d'origine, puis vérifier le retour exact de la
   trame. À refaire pour chaque jour que l'on voudra valider.
3. **Circulateur** : pour une plage ajoutée au jour courant, vérifier le
   passage de `TEST — Bouclage ECS brut` à `1` au début de la plage, puis à
   `0` après la fin. Comparer avec la pompe réelle (bruit, température du
   retour) et noter le délai.
4. Noter si le panneau écrit toujours des plages **triées et compactes**
   (par exemple `31 43 70 71 AC AD FF FF` et non `31 43 AC AD 70 71 FF FF`).
   VissLink ne modifiera que des blocs de cette forme.

## 5. Contrôle « aucune écriture »

1. Appuyer une fois sur « Bouclage ECS — Démarrer maintenant ». Attendu :
   « Bouclage ECS — État de l'opération » affiche « Refusé : écritures de
   programmation désactivées … ». Aucune trame n'est émise.
2. En fin d'essai, vérifier le journal :

   ```sh
   grep -c -- ' -> ' essai-v05.log        # trames d'écriture : doit afficher 0
   grep -c 'refused' essai-v05.log        # commandes refusées (informatif)
   ```

3. Comparer une dernière fois le panneau au relevé papier : tout doit être
   identique, aux modifications de l'étape 4 près, que vous aurez annulées
   vous-même.

## 6. Retour à la production

Reflasher [`examples/iot-viessmann.yaml`](../examples/iot-viessmann.yaml),
puis supprimer dans Home Assistant les entités `TEST — …` devenues
orphelines.

## Résultats à rapporter

| Point | À noter |
|---|---|
| Resynchro | écart mesuré entre deux lectures (étape 1) ; intervalle Éco/Party retenu |
| Horloge | trame brute, concordance date/heure/jour, écart avec Home Assistant |
| Éco/Party | valeurs observées pour chaque action, indépendance A1/M2, fin automatique de Party |
| Programmations | jours dont la correspondance est confirmée, forme des blocs écrits par le panneau |
| Circulateur | valeurs de `0x0846`, délai de démarrage et d'arrêt, concordance avec la pompe |
| Écritures | résultat de `grep -c ' -> '` (attendu : 0) |

Ces résultats conditionnent `validated_days` et la phase 2 du bouclage à la
demande ([`bouclage-ecs-v0.5.md`](bouclage-ecs-v0.5.md) § 8). Ils ne
l'autorisent pas.
