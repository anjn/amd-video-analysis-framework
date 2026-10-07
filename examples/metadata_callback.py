"""Metadata-only amdpython example; no image mapping or GPU access."""
def process(frame):
    people = [obj for obj in frame['objects'] if obj['label'] == 'person']
    return {'people': len(people),
            'tracked_people': [obj['tracking_id'] for obj in people if 'tracking_id' in obj]}
