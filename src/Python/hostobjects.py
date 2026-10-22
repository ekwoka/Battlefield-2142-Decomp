# The engine objects the host module hands to scripts (BF2142.exe wraps an
# engine object in a Python object holding a "token"; the methods are the
# names it registers: cp_getParam, getTeam, getOccupyingPlayers...).
# One Python object per engine object, so scripts can keep attributes on
# them (gpm_cq.py sets cp.triggerId, cp.flagPosition...).
import host

_objects = {}

_METHODS = (
	'cp_getParam', 'cp_setParam', 'getTeam', 'setTeam', 'getIsWreck', 'getIsRemoteControlled',
	'getOccupyingPlayers', 'isTitan', 'isValid', 'getTemplateProperty', 'setDamage', 'getMaxDamage',
	'getDamage', 'getParent', 'getChildren', 'isControlPoint', 'isPlayerControlObject', 'hasArmor',
	'getPosition', 'setPosition', 'getRotation', 'setRotation',
)


class PhysicalObject(object):
	def __init__(self, token):
		self.token = token

	@property
	def templateName(self):
		return host.obj_call(self.token, 'templateName')

	def __repr__(self):
		return '<PhysicalObject %d %s>' % (self.token, self.templateName)


def _method(name):
	def call(self, *args):
		return host.obj_call(self.token, name, *args)
	call.__name__ = name
	return call


for _name in _METHODS:
	setattr(PhysicalObject, _name, _method(_name))


def wrap(token):
	"""The Python object of an engine object (created once)."""
	obj = _objects.get(token)
	if obj is None:
		obj = _objects[token] = PhysicalObject(token)
	return obj


def forget(token):
	_objects.pop(token, None)
