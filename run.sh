#!/bin/sh -e

cd $(dirname "$0")

export LD_LIBRARY_PATH="$PWD/lib"

make

export color_threshold=20 # variation de luminosité d'un pixel entre deux images consécutives pour le considérer différent. Une grande valeur filtre les mouvements flous (pluie, ensoleillement). Entre 0 et 255.
export pixels_threshold=100 # nombre de pixels différents dans deux images consécutives pour déclencher une alerte. Un grand nombre filtre les mouvements localisés (bruit, feuille).
export audio_threshold=2e4 # puissance sonore pour déclencher une alerte. Une grande valeur réduit la sensibilité au bruit ambiant.
export alert_threshold=10 # nombre d'images où on doit observer du mouvement avant de déclencher une alerte. Un grand nombre filtre les mouvements localisés (bruit, feuille).
export relaxation_time=100 # nombre d'images sans mouvement avant d'annuler une alerte. Un grand nombre permet de détecter les mouvements très lents.
export mask=mask.png # image blanche là où on cherche du mouvement et noire ailleurs. Peindre cette image en noir sur la route et la végétation pour éviter les fausses alertes.

time ./main *.MPEG | tee results.txt
