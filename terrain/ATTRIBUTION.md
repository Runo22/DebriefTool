# Data attribution

`data/terrain.tdem` is a derived work of:

- **Copernicus DEM GLO-30 and GLO-90** — © DLR e.V. 2010–2014 and © Airbus
  Defence and Space GmbH 2014–2018, provided under COPERNICUS by the European
  Union and ESA; all rights reserved. Free of charge for any use, including
  redistribution of derived products, under the Copernicus DEM licence
  (see the Copernicus DEM product page on the Copernicus Data Space). Required
  notice: "produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus
  Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European
  Union and ESA; all rights reserved".
  Tiles obtained from the AWS Open Data registry (`copernicus-dem-30m`,
  `copernicus-dem-90m`).

- **OurAirports** runway data (`tools/runways_tr.csv`) — public domain,
  <https://ourairports.com/data/>.

Modifications: resampled to the layer grids, runways flattened, heights
quantised (0.1–1 m) and re-encoded.
