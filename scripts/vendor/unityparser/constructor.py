import collections.abc
from .constants import OrderedFlowDict

from yaml.constructor import Constructor as YamlConstructor, ConstructorError
from yaml.nodes import MappingNode


class uniqstr(str):
    def __new__(cls, content):
        return super().__new__(cls, content)


class DupSeq(list):
    """LOCAL PATCH: 重复键收集器。Unity 4.x 旧序列化格式（serializedVersion<=3）
    的 m_TexEnvs/m_Floats/m_Colors 用重复的 `data:` 键表达键值对列表，
    PyYAML/unityparser 默认 `mapping[key] = value` 会静默丢弃前面的项
    （实测 7 个贴图槽只剩最后 1 个）。重复键改为收集进 DupSeq 列表，
    上层按需归一化。"""
    dupseq = True


class Constructor(YamlConstructor):

    def __init__(self, register=None):
        super(Constructor, self).__init__()
        self.register = register

    def construct_mapping(self, node, deep=False):
        if not isinstance(node, MappingNode):
            raise ConstructorError(None, None,
                                   "expected a mapping node, but found %s" % node.id,
                                   node.start_mark)
        # UNITY: dict has to be ordered to reproduce nested maps, also save flow style
        mapping = OrderedFlowDict()
        mapping.set_flow_style(node.flow_style)
        for key_node, value_node in node.value:
            key = self.construct_object(key_node, deep=deep)
            if not isinstance(key, collections.abc.Hashable):
                raise ConstructorError("while constructing a mapping", node.start_mark,
                                       "found unhashable key", key_node.start_mark)
            value = self.construct_object(value_node, deep=deep)
            # LOCAL PATCH: duplicate keys are legal in legacy Unity serialised
            # files; collect them instead of silently overwriting.
            if key in mapping:
                prev = mapping[key]
                if isinstance(prev, DupSeq):
                    prev.append(value)
                else:
                    mapping[key] = DupSeq([prev, value])
            else:
                mapping[key] = value
        return mapping

    def construct_scalar(self, node):
        value = super().construct_scalar(node)
        if isinstance(value, str) and node.style is not None:
            value = uniqstr(value)
            self.register.set(value, node.style)
        return value
