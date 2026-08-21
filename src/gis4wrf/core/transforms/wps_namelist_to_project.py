# GIS4WRF (https://doi.org/10.5281/zenodo.1288569)
# Copyright (c) 2018 D. Meyer and M. Riechert. Licensed under MIT.

from typing import List

from gis4wrf.core.util import export
from gis4wrf.core.errors import UserError, UnsupportedError
from gis4wrf.core.project import Project

@export
def convert_wps_nml_to_project(nml: dict, existing_project: Project) -> Project:
    data = existing_project.data.copy()
    try:
        data['domains'] = convert_nml_to_project_domains(nml)
    except KeyError as e:
        raise UserError(f'Invalid namelist, section/variable {e} not found')
    project = Project(data, existing_project.path)
    return project

def convert_nml_to_project_domains(nml: dict) -> List[dict]:
    max_dom = nml['share']['max_dom'] # type: int

    nml = nml['geogrid']
    map_proj = nml['map_proj'] # type: str
    parent_id = nml['parent_id'] # type: List[int]
    parent_grid_ratio = nml['parent_grid_ratio'] # type: List[int]
    i_parent_start = nml['i_parent_start'] # type: List[int]
    j_parent_start = nml['j_parent_start'] # type: List[int]
    e_we = nml['e_we'] # type: List[int]
    e_sn = nml['e_sn'] # type: List[int]
    dx = nml['dx'] # type: float
    dy = nml['dy'] # type: float
    ref_lon = nml['ref_lon'] # type: float
    ref_lat = nml['ref_lat'] # type: float
    truelat1 = nml.get('truelat1') # type: float
    truelat2 = nml.get('truelat2') # type: float
    stand_lon = nml.get('stand_lon', 0.0) # type: float

    if len(parent_id) != max_dom:
        raise UserError(
            f'max_dom ({max_dom}) does not match the number of domains actually defined '
            f'({len(parent_id)}) in the geogrid section\'s arrays')

    if parent_id[0] != 1:
        raise UserError("The first domain must be the root domain (parent_id(1) must be 1)")

    for i in range(1, max_dom):
        if not (1 <= parent_id[i] <= i):
            raise UserError(
                f'Domain {i + 1} has an invalid parent_id ({parent_id[i]}): it must refer to an '
                'earlier, already-defined domain (WPS requires parent domains to be numbered '
                'before their children)')

    # Check whether ref_x/ref_y is omitted, so that we can assume ref == center.
    if 'ref_x' in nml or 'ref_y' in nml:
        raise UnsupportedError('ref_x/ref_y is not supported in namelist')

    if map_proj == 'lat-lon' and stand_lon != 0.0:
        raise UnsupportedError('Rotated lat-lon projection is not supported')
    if map_proj not in ('lat-lon', 'lambert', 'mercator', 'polar'):
        raise UnsupportedError(f'Map projection "{map_proj}" is not supported')

    # e_we/e_sn represent the number of velocity points (i.e. u-staggered or
    # v-staggered points), which is one more than the number of cells.
    cols = [e - 1 for e in e_we]
    rows = [e - 1 for e in e_sn]

    root = {
        'map_proj': map_proj,
        'parent_id': 1,
        'cell_size': [dx, dy],
        'center_lonlat': [ref_lon, ref_lat],
        'domain_size': [cols[0], rows[0]],
    }
    if truelat1 is not None:
        root['truelat1'] = truelat1
    if truelat2 is not None:
        root['truelat2'] = truelat2
    if stand_lon is not None:
        root['stand_lon'] = stand_lon

    domains = [root]
    for i in range(1, max_dom):
        domains.append({
            'parent_id': parent_id[i],
            'parent_cell_size_ratio': parent_grid_ratio[i],
            'padding_left': i_parent_start[i] - 1,
            'padding_bottom': j_parent_start[i] - 1,
            'domain_size': [cols[i], rows[i]],
        })

    return domains
